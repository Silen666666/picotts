# -*- coding: utf-8 -*-
"""
PC-Simulation (CPython 3) des Regelkerns mit einem einfachen thermischen
Raummodell. Dient zum Prüfen/Anpassen der Reglerparameter vor der Inbetriebnahme.

Aufruf:  python3 tools/simulation.py   (optional: --plot, benötigt matplotlib)

Modell (vereinfachtes RC-Modell; Parameter sind grobe Annahmen für einen
20-m²-Raum mit 6,5 cm Zementestrich, KEINE Messwerte – an das eigene Haus anpassen):
  Estrich:  C_e * dT_e/dt = P_ventil - (T_e - T_r)/R_er
  Raum:     C_r * dT_r/dt = (T_e - T_r)/R_er - (T_r - T_aussen)/R_ra
  Ventil:   thermischer Antrieb, 3 min Laufzeit (linearer Hub-Anstieg)
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "firmware", "zentrale"))
from regler import Regler  # noqa: E402


def simuliere(tage=5, dt=10.0, t_aussen=0.0, plot=False):
    zonen = {"test": {"ausgang": 0, "soll": 21.0, "absenkung": ("22:00", "06:00", 2.0)}}
    r = Regler(zonen, 1, periode_s=1200)

    # Modellparameter (Annahmen)
    # Estrich: 20 m² * 0,065 m * 2000 kg/m³ * 1000 J/(kg K) ≈ 2,6 MJ/K
    C_e, C_r = 2.6e6, 1.0e6           # J/K   Estrich, Raumluft+Möbel (Annahme)
    # Fußboden->Raum ca. 10 W/(m² K) * 20 m² = 200 W/K  -> R = 0,005 K/W
    R_er, R_ra = 0.005, 0.030         # K/W   Raum->außen ca. 33 W/K (Annahme)
    P_max = 1600.0                    # W bei voll offenem Ventil (80 W/m²)
    T_e = T_r = 17.0
    hub = 0.0                         # 0..1 Ventilhub

    log = []
    t = 0.0
    while t < tage * 86400:
        if int(t) % 60 == 0:
            r.messwert("test", T_r, t)          # Sensor sendet jede Minute
        minute = int((t // 60) % 1440)
        aus, _ = r.schritt(t, minute)
        hub += (dt / 180.0) * (1 if aus[0] else -1)
        hub = min(max(hub, 0.0), 1.0)
        P = P_max * hub
        q_er = (T_e - T_r) / R_er
        q_ra = (T_r - t_aussen) / R_ra
        T_e += dt * (P - q_er) / C_e
        T_r += dt * (q_er - q_ra) / C_r
        if int(t) % 600 == 0:
            z = r.zonen["test"]
            log.append((t / 3600.0, T_r, z.soll, z.stellgrad))
        t += dt

    # Kennzahlen am letzten Tag (eingeschwungen)
    letzter = [x for x in log if x[0] >= (tage - 1) * 24 and 8 <= x[0] % 24 < 21]
    abw = [abs(x[1] - x[2]) for x in letzter]
    print("Tag %d, 08-21 Uhr: mittlere |Abweichung| = %.2f K, max = %.2f K"
          % (tage, sum(abw) / len(abw), max(abw)))

    if plot:
        import matplotlib.pyplot as plt
        h = [x[0] for x in log]
        fig, ax = plt.subplots(2, 1, sharex=True, figsize=(10, 6))
        ax[0].plot(h, [x[1] for x in log], label="Raum ist")
        ax[0].plot(h, [x[2] for x in log], "--", label="Soll")
        ax[0].set_ylabel("°C")
        ax[0].legend()
        ax[1].step(h, [x[3] * 100 for x in log], where="post")
        ax[1].set_ylabel("Stellgrad %")
        ax[1].set_xlabel("Stunden")
        plt.tight_layout()
        plt.savefig(os.path.join(os.path.dirname(__file__), "simulation.png"), dpi=120)
        print("Grafik gespeichert: tools/simulation.png")
    return log


if __name__ == "__main__":
    simuliere(plot="--plot" in sys.argv)
