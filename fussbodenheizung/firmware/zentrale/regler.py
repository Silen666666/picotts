# -*- coding: utf-8 -*-
"""
Regelkern der Fußbodenheizungsregelung.

Läuft unverändert unter MicroPython (ESP32, Zentrale in der Unterverteilung)
und unter CPython (PC-Simulation und Tests, siehe tools/ und tests/).
Darum: keine MicroPython-spezifischen Imports in dieser Datei.

Prinzip je Zone (= Raum mit einem oder mehreren Heizkreisen/Stellantrieben):
  * PI-Regler berechnet einen Stellgrad 0..1 (Ventil-Öffnungsanteil).
  * Thermische Stellantriebe kennen nur AUF/ZU. Der Stellgrad wird darum als
    langsame Pulsweitenmodulation (PWM) ausgegeben, Periode typ. 20 min.
    (Thermische Antriebe brauchen ca. 3 min zum Öffnen/Schließen.)
  * Die PWM-Perioden der Zonen sind zeitlich versetzt, damit nicht alle
    Antriebe gleichzeitig einschalten (Einschaltstrom).
  * Sicherheit/Komfort:
      - Fühlerausfall (kein Messwert seit SENSOR_TIMEOUT) -> Ersatz-Stellgrad
      - Frostschutz (Raumtemperatur < FROST_C)            -> Ventil voll auf
      - Ventilschutz: Ventil war >= 7 Tage nicht offen      -> 10 min öffnen
"""


def _begrenze(x, lo, hi):
    if x < lo:
        return lo
    if x > hi:
        return hi
    return x


def sollwert_aktuell(zone_cfg, minuten_des_tages):
    """Sollwert einer Zone unter Berücksichtigung der Nachtabsenkung.

    zone_cfg: dict mit 'soll' und optional 'absenkung' = (von_hh_mm, bis_hh_mm, delta_K)
              z.B. ("22:00", "05:30", 2.0)
    minuten_des_tages: 0..1439 oder None (keine gültige Uhrzeit -> keine Absenkung)
    """
    soll = zone_cfg["soll"]
    ab = zone_cfg.get("absenkung")
    if ab is None or minuten_des_tages is None:
        return soll

    def _m(txt):
        h, m = txt.split(":")
        return int(h) * 60 + int(m)

    von, bis, delta = _m(ab[0]), _m(ab[1]), ab[2]
    t = minuten_des_tages
    if von <= bis:
        aktiv = von <= t < bis
    else:  # über Mitternacht
        aktiv = t >= von or t < bis
    return soll - delta if aktiv else soll


class Zone:
    def __init__(self, name, ausgang, kp=0.5, tn_s=7200.0, periode_s=1200.0,
                 versatz_s=0.0, min_anteil=0.10, sensor_timeout_s=900.0,
                 ersatz_stellgrad=0.30, frost_c=8.0,
                 ventilschutz_intervall_s=7 * 24 * 3600.0,
                 ventilschutz_dauer_s=600.0):
        self.name = name
        self.ausgang = ausgang          # Index des Relaisausgangs K1..Kn (0-basiert)
        self.kp = kp                    # Stellgrad pro Kelvin Regelabweichung
        self.tn_s = tn_s                # Nachstellzeit
        self.periode_s = periode_s
        self.versatz_s = versatz_s
        self.min_anteil = min_anteil    # kürzere Pulse lohnen bei therm. Antrieben nicht
        self.sensor_timeout_s = sensor_timeout_s
        self.ersatz_stellgrad = ersatz_stellgrad
        self.frost_c = frost_c
        self.vs_intervall = ventilschutz_intervall_s
        self.vs_dauer = ventilschutz_dauer_s

        self.ist = None                 # letzter Messwert in °C
        self.ist_zeit = None            # Zeitpunkt des letzten Messwerts (s)
        self.soll = 20.0
        self.integral = 0.0
        self.stellgrad = 0.0
        self.status = "init"
        self._periode_start = None
        self._ein_bis = 0.0
        self._letzt_offen = None
        self._vs_bis = 0.0

    # ------------------------------------------------------------------
    def messwert(self, temp_c, jetzt_s):
        self.ist = temp_c
        self.ist_zeit = jetzt_s

    def _berechne_stellgrad(self, jetzt_s):
        """Einmal pro PWM-Periode aufgerufen."""
        if self.ist is None or self.ist_zeit is None or \
                jetzt_s - self.ist_zeit > self.sensor_timeout_s:
            self.status = "FUEHLER_AUSFALL"
            return self.ersatz_stellgrad

        if self.ist < self.frost_c:
            self.status = "FROSTSCHUTZ"
            return 1.0

        e = self.soll - self.ist
        p = self.kp * e
        # Integralanteil mit Anti-Windup (nur integrieren, wenn nicht in Sättigung
        # in dieselbe Richtung)
        roh = p + self.integral
        if not ((roh >= 1.0 and e > 0) or (roh <= 0.0 and e < 0)):
            self.integral += self.kp * e * self.periode_s / self.tn_s
            self.integral = _begrenze(self.integral, 0.0, 1.0)
        y = _begrenze(p + self.integral, 0.0, 1.0)

        if y < self.min_anteil:
            y = 0.0
        elif y > 1.0 - self.min_anteil:
            y = 1.0
        self.status = "REGELUNG"
        return y

    def schritt(self, jetzt_s):
        """Liefert True, wenn der Stellantrieb dieser Zone bestromt werden soll."""
        if self._letzt_offen is None:
            self._letzt_offen = jetzt_s

        if self._periode_start is None:
            # Erste Periode um den Versatz verzögert beginnen
            self._periode_start = jetzt_s + self.versatz_s - self.periode_s
        if jetzt_s >= self._periode_start + self.periode_s:
            self._periode_start = jetzt_s
            self.stellgrad = self._berechne_stellgrad(jetzt_s)
            self._ein_bis = jetzt_s + self.stellgrad * self.periode_s

        ein = jetzt_s < self._ein_bis

        # Ventilschutz (Festsitzen verhindern, z.B. im Sommer)
        if not ein and jetzt_s - self._letzt_offen >= self.vs_intervall:
            self._vs_bis = jetzt_s + self.vs_dauer
            self._letzt_offen = jetzt_s
        if jetzt_s < self._vs_bis:
            ein = True
            self.status = "VENTILSCHUTZ"

        if ein:
            self._letzt_offen = jetzt_s
        return ein


class Regler:
    """Verwaltet alle Zonen und berechnet die Ausgänge."""

    def __init__(self, zonen_cfg, anzahl_ausgaenge, periode_s=1200.0):
        self.anzahl = anzahl_ausgaenge
        self.zonen = {}
        n = len(zonen_cfg)
        for i, (name, cfg) in enumerate(zonen_cfg.items()):
            z = Zone(name, cfg["ausgang"],
                     kp=cfg.get("kp", 0.5),
                     tn_s=cfg.get("tn_s", 7200.0),
                     periode_s=periode_s,
                     versatz_s=i * periode_s / max(n, 1))
            z.soll = cfg["soll"]
            self.zonen[name] = z
        self.cfg = zonen_cfg

    def messwert(self, zone, temp_c, jetzt_s):
        z = self.zonen.get(zone)
        if z is not None:
            z.messwert(temp_c, jetzt_s)
            return True
        return False

    def schritt(self, jetzt_s, minuten_des_tages=None):
        """Gibt (ausgaenge: list[bool], waermeanforderung: bool) zurück."""
        aus = [False] * self.anzahl
        for name, z in self.zonen.items():
            z.soll = sollwert_aktuell(self.cfg[name], minuten_des_tages)
            if z.schritt(jetzt_s):
                # mehrere Zonen dürfen sich keinen Ausgang teilen; Prüfung in config
                aus[z.ausgang] = True
        anforderung = any(z.stellgrad > 0 for z in self.zonen.values())
        return aus, anforderung

    def statuszeilen(self):
        zeilen = []
        for name, z in self.zonen.items():
            ist = "--.-" if z.ist is None else "%4.1f" % z.ist
            zeilen.append("%-12s K%d ist=%s soll=%4.1f y=%3d%% %s" % (
                name, z.ausgang + 1, ist, z.soll, int(z.stellgrad * 100), z.status))
        return zeilen
