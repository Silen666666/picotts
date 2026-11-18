# -*- coding: utf-8 -*-
"""
Zentrale der Fußbodenheizungsregelung (MicroPython, ESP32 mit externer Antenne,
z.B. ESP32-DevKitC-32U) – sitzt in der Unterverteilung auf einem Hutschienen-
Trägergehäuse.

Ablauf:
  1. Ausgänge sicher AUS (alle Stellantriebe stromlos = Ventile ZU bei NC-Antrieben)
  2. optional WLAN + NTP (nur für Nachtabsenkung)
  3. ESP-NOW-Empfang der Raumtemperaturen
  4. alle REGEL_TAKT_S: Regler rechnen, Relais setzen, Watchdog füttern

Dateien auf dem ESP32:  main.py, config.py, regler.py
"""
import time
import json
import machine
import network
import espnow

import config
from regler import Regler

# ---------------------------------------------------------------- Ausgänge
relais = [machine.Pin(g, machine.Pin.OUT, value=0) for g in config.RELAIS_GPIO]
anforderung = None
if config.ANFORDERUNG_GPIO is not None:
    anforderung = machine.Pin(config.ANFORDERUNG_GPIO, machine.Pin.OUT, value=0)
led = None
if config.STATUS_LED_GPIO is not None:
    led = machine.Pin(config.STATUS_LED_GPIO, machine.Pin.OUT, value=0)

# Plausibilitätsprüfung der Konfiguration
_benutzt = {}
for _n, _c in config.ZONEN.items():
    a = _c["ausgang"]
    if not 0 <= a < len(relais):
        raise ValueError("Zone %s: Ausgang %d existiert nicht" % (_n, a))
    if a in _benutzt:
        raise ValueError("Ausgang K%d doppelt: %s/%s" % (a + 1, _n, _benutzt[a]))
    _benutzt[a] = _n

wdt = machine.WDT(timeout=60000)   # Reset, falls die Schleife hängt


# ---------------------------------------------------------------- Zeit
def _letzter_sonntag(jahr, monat):
    # Tag (1..31) des letzten Sonntags im Monat
    for tag in range(31, 24, -1):
        try:
            t = time.mktime((jahr, monat, tag, 0, 0, 0, 0, 0))
        except (OverflowError, ValueError):
            continue
        if time.localtime(t)[1] == monat and time.localtime(t)[6] == 6:
            return tag
    return 31


def minuten_lokal(zeit_gueltig):
    """Minuten seit Mitternacht MEZ/MESZ oder None ohne gültige Uhrzeit."""
    if not zeit_gueltig:
        return None
    utc = time.time()
    j = time.localtime(utc)[0]
    beginn = time.mktime((j, 3, _letzter_sonntag(j, 3), 1, 0, 0, 0, 0))
    ende = time.mktime((j, 10, _letzter_sonntag(j, 10), 1, 0, 0, 0, 0))
    offset = 7200 if beginn <= utc < ende else 3600
    lt = time.localtime(utc + offset)
    return lt[3] * 60 + lt[4]


# ---------------------------------------------------------------- Funk
sta = network.WLAN(network.STA_IF)
sta.active(True)
zeit_gueltig = False

if config.WLAN_SSID:
    sta.connect(config.WLAN_SSID, config.WLAN_PASSWORT)
    for _ in range(40):
        if sta.isconnected():
            break
        time.sleep_ms(500)
        wdt.feed()
    if sta.isconnected():
        try:
            import ntptime
            ntptime.host = config.NTP_SERVER
            ntptime.settime()        # setzt die RTC auf UTC
            zeit_gueltig = True
        except Exception as ex:
            print("NTP fehlgeschlagen:", ex)
    else:
        print("WLAN nicht erreichbar – Regelung ohne Uhrzeit")
else:
    sta.disconnect()
    try:
        sta.config(channel=config.ESPNOW_KANAL)
    except Exception as ex:
        print("Kanal setzen fehlgeschlagen:", ex)

try:
    # Energiesparmodus aus, sonst gehen bei aktivem WLAN ESP-NOW-Pakete verloren
    sta.config(pm=sta.PM_NONE)
except Exception:
    pass

en = espnow.ESPNow()
en.active(True)

print("Zentrale MAC:", ":".join("%02x" % b for b in sta.config("mac")))
try:
    print("Funkkanal:", sta.config("channel"))
except Exception:
    pass

# ---------------------------------------------------------------- Regler
regler = Regler(config.ZONEN, len(relais), periode_s=config.PWM_PERIODE_S)

_t0 = time.ticks_ms()
_uptime_ms = 0


def jetzt_s():
    """Monotone Laufzeit in Sekunden (überlaufsicher)."""
    global _t0, _uptime_ms
    t = time.ticks_ms()
    _uptime_ms += time.ticks_diff(t, _t0)
    _t0 = t
    return _uptime_ms / 1000.0


def mac_text(mac):
    return ":".join("%02x" % b for b in mac)


naechster_takt = 0.0
letzte_ntp_sync = jetzt_s()

while True:
    # ---- Messwerte empfangen (blockiert max. 1 s)
    try:
        host, msg = en.recv(1000)
    except OSError as ex:
        host, msg = None, None
        print("ESP-NOW Fehler:", ex)
    if msg:
        m = mac_text(host)
        zone = config.SENSOREN.get(m)
        if zone is None:
            print("Unbekannter Sensor", m, msg)
        else:
            try:
                d = json.loads(msg)
                t = float(d["t"])
                if -20.0 < t < 50.0:            # Plausibilitätsgrenze
                    regler.messwert(zone, t, jetzt_s())
            except Exception as ex:
                print("Defektes Paket von", m, ex)

    # ---- Regeltakt
    t = jetzt_s()
    if t >= naechster_takt:
        naechster_takt = t + config.REGEL_TAKT_S
        aus, anf = regler.schritt(t, minuten_lokal(zeit_gueltig))
        for pin, wert in zip(relais, aus):
            pin.value(1 if wert else 0)
        if anforderung is not None:
            anforderung.value(1 if anf else 0)
        if led is not None:
            led.value(not led.value())          # Blinken = Schleife läuft
        for zeile in regler.statuszeilen():
            print(zeile)
        print("-")

        # NTP einmal täglich nachziehen (RTC des ESP32 driftet)
        if zeit_gueltig and t - letzte_ntp_sync > 86400 and sta.isconnected():
            try:
                import ntptime
                ntptime.settime()
            except Exception:
                pass
            letzte_ntp_sync = t

    wdt.feed()
