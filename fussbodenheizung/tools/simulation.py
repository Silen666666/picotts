# -*- coding: utf-8 -*-
"""
PC-Simulation (Python 3) der Zonenregelung, wie sie in esphome/common/zone.yaml
auf der Zentrale läuft – zum Abschätzen/Anpassen der Reglerparameter kp/ki.

Nachgebildet nach dem ESPHome-Quellcode (Version 2026.11):
  * components/pid/pid_controller.cpp   P = kp*e, I += ki*e*dt (begrenzt auf
    min_integral..max_integral), Aufruf bei jedem neuen Sensorwert
  * components/slow_pwm/slow_pwm_output.cpp   EIN, solange level*Periode größer
    als die seit Periodenbeginn vergangene Zeit ist
  * Sicherheitslogik aus zone.yaml (Intervall 30 s): Mindestanteil 10 %,
    Fühlerausfall -> 30 %, Frostschutz < 8 °C

Raummodell: vereinfachtes RC-Modell; Parameter sind ANNAHMEN für einen 20-m²-Raum
mit 6,5 cm Zementestrich – keine Messwerte.

Aufruf:  python3 tools/simulation.py [--plot] [--kp 0.5] [--tn 7200]
"""
import argparse
import math
import os


class EsphomePID:
    """Heizungs-Anteil des ESPHome-PID-Reglers (ohne Totband, kd = 0)."""

    def __init__(self, kp, ki, min_integral=0.0, max_integral=1.0):
        self.kp, self.ki = kp, ki
        self.min_i, self.max_i = min_integral, max_integral
        self.integral = 0.0
        self.letzte_zeit = None

    def update(self, soll, ist, jetzt):
        dt = 0.0 if self.letzte_zeit is None else jetzt - self.letzte_zeit
        self.letzte_zeit = jetzt
        e = soll - ist
        self.integral = min(max(self.integral + e * dt * self.ki, self.min_i), self.max_i)
        u = self.kp * e + self.integral
        return min(max(u, 0.0), 1.0)          # write_output_: nur Heizen, max. 1


def sicherheitslogik(y_pid, ist, aus=False, frost=8.0, ersatz=0.30, min_anteil=0.10):
    """Entspricht dem Lambda im interval-Block von zone.yaml."""
    if ist is not None and not math.isnan(ist) and ist < frost:
        return 1.0, "Frostschutz"
    if aus:
        return 0.0, "Aus"
    if ist is None or math.isnan(ist):
        return ersatz, "Fühlerausfall"
    if y_pid < min_anteil:
        return 0.0, "Regelung"
    if y_pid > 1.0 - min_anteil:
        return 1.0, "Regelung"
    return y_pid, "Regelung"


def soll_zeitplan(stunde, komfort=21.0, absenk=19.0, von=6.0, bis=22.0):
    """Wie die HA-Automation: Komfort innerhalb der Zeitplan-Zeiten, sonst Absenkung."""
    return komfort if von <= stunde < bis else absenk


def simuliere(tage=5, kp=0.8, tn_s=10800.0, t_aussen=0.0, ausfall=None, plot=False):
    ki = kp / tn_s
    pid = EsphomePID(kp, ki)
    periode = 1200.0                       # slow_pwm period: 20min
    C_e, C_r = 2.6e6, 1.0e6                # J/K   Estrich, Raumluft+Möbel (Annahme)
    R_er, R_ra = 0.005, 0.030              # K/W   (Annahmen)
    P_max = 1600.0                         # W     (80 W/m², Annahme)
    T_e = T_r = 17.0
    hub = 0.0
    y_pid, y, status = 0.0, 0.0, ""
    periode_start = 0.0
    letzter_wert = None
    log = []
    dt = 10.0
    t = 0.0
    while t < tage * 86400:
        soll = soll_zeitplan((t / 3600.0) % 24)
        sensor_tot = ausfall is not None and ausfall[0] <= t / 3600.0 < ausfall[1]
        if int(t) % 60 == 0 and not sensor_tot:            # Raumsensor alle 60 s
            letzter_wert = t
            y_pid = pid.update(soll, T_r, t)
        ist = T_r if (letzter_wert is not None and t - letzter_wert <= 900) else float("nan")
        if int(t) % 30 == 0:                                # interval: 30s
            y, status = sicherheitslogik(y_pid, ist)
        if t - periode_start >= periode:                     # slow_pwm
            periode_start += periode
        ein = y * periode > t - periode_start
        hub = min(max(hub + (dt / 180.0) * (1 if ein else -1), 0.0), 1.0)
        q_er = (T_e - T_r) / R_er
        q_ra = (T_r - t_aussen) / R_ra
        T_e += dt * (P_max * hub - q_er) / C_e
        T_r += dt * (q_er - q_ra) / C_r
        if int(t) % 600 == 0:
            log.append((t / 3600.0, T_r, soll, y, status))
        t += dt

    letzter = [x for x in log if x[0] >= (tage - 1) * 24 and 8 <= x[0] % 24 < 21]
    abw = [abs(x[1] - x[2]) for x in letzter]
    print("kp=%.2f Tn=%.0f s: Tag %d 08-21 Uhr: mittlere |Abweichung| %.2f K, max %.2f K"
          % (kp, tn_s, tage, sum(abw) / len(abw), max(abw)))

    if plot:
        import matplotlib.pyplot as plt
        h = [x[0] for x in log]
        fig, ax = plt.subplots(2, 1, sharex=True, figsize=(10, 6))
        ax[0].plot(h, [x[1] for x in log], label="Raum ist")
        ax[0].plot(h, [x[2] for x in log], "--", label="Soll (HA-Zeitplan)")
        ax[0].set_ylabel("°C")
        ax[0].legend()
        ax[0].set_title("Simulation ESPHome-PID + slow_pwm (Modellannahmen, keine Messung)")
        ax[1].step(h, [x[3] * 100 for x in log], where="post")
        ax[1].set_ylabel("Stellgrad %")
        ax[1].set_xlabel("Stunden")
        if ausfall:
            for a in ax:
                a.axvspan(ausfall[0], ausfall[1], color="orange", alpha=0.2)
        plt.tight_layout()
        pfad = os.path.join(os.path.dirname(os.path.abspath(__file__)), "simulation.png")
        plt.savefig(pfad, dpi=120)
        print("Grafik gespeichert:", os.path.normpath(pfad))
    return log


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--plot", action="store_true")
    ap.add_argument("--kp", type=float, default=0.8)
    ap.add_argument("--tn", type=float, default=10800.0)
    a = ap.parse_args()
    # Beispiel: am Tag 3 fällt der Raumsensor für 6 h aus (orange markiert)
    simuliere(kp=a.kp, tn_s=a.tn, ausfall=(60, 66), plot=a.plot)
