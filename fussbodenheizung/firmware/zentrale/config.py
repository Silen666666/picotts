# -*- coding: utf-8 -*-
"""Konfiguration der Zentrale (ESP32 in der Unterverteilung). Bitte anpassen."""

# --- WLAN (optional, nur für Uhrzeit/NTP -> Nachtabsenkung) -----------------
# Leer lassen ("") = kein WLAN, dann arbeitet die Regelung ohne Absenkung.
WLAN_SSID = ""
WLAN_PASSWORT = ""
NTP_SERVER = "pool.ntp.org"

# ESP-NOW-Kanal, falls KEIN WLAN genutzt wird (1..13). Mit WLAN übernimmt das
# ESP32 automatisch den Kanal des Routers; die Raumsensoren suchen den Kanal
# selbstständig (siehe raumsensor/main.py).
ESPNOW_KANAL = 6

# --- Ausgänge (GPIO -> ULN2803 -> Koppelrelais K1..K8, K9 = Wärmeanforderung)
# Nur GPIOs, die keine Strapping-Pins sind (nicht 0, 2, 5, 12, 15) und die beim
# Booten LOW bleiben.
RELAIS_GPIO = [16, 17, 18, 19, 21, 22, 23, 25]   # K1..K8
ANFORDERUNG_GPIO = 26                             # K9 (optional, potentialfrei)
STATUS_LED_GPIO = 27                              # LED an der Frontplatte (optional)

# --- Regelparameter -----------------------------------------------------------
PWM_PERIODE_S = 1200      # 20 min
REGEL_TAKT_S = 10         # Schrittweite der Hauptschleife

# --- Raumsensoren: MAC-Adresse -> Zone -------------------------------------
# Die MAC wird beim ersten Start des Raumsensors auf der seriellen Konsole
# ausgegeben. Nur hier eingetragene Sensoren werden akzeptiert.
SENSOREN = {
    # "aa:bb:cc:dd:ee:01": "wohnen",
    # "aa:bb:cc:dd:ee:02": "kueche",
}

# --- Zonen ---------------------------------------------------------------------
# ausgang: 0 = K1 ... 7 = K8. Mehrere Heizkreise eines Raums werden am selben
# Relais parallel angeschlossen (Klemmenplan im README).
# absenkung: (von, bis, Kelvin) – wirkt nur mit gültiger Uhrzeit (WLAN/NTP).
ZONEN = {
    "wohnen":  {"ausgang": 0, "soll": 21.0, "absenkung": ("22:30", "05:30", 2.0)},
    "kueche":  {"ausgang": 1, "soll": 20.5, "absenkung": ("22:30", "05:30", 2.0)},
    "bad":     {"ausgang": 2, "soll": 23.0, "absenkung": ("23:00", "05:00", 1.0)},
    "schlafen": {"ausgang": 3, "soll": 18.0},
    "kind":    {"ausgang": 4, "soll": 20.0, "absenkung": ("21:00", "06:00", 2.0)},
    "buero":   {"ausgang": 5, "soll": 20.5, "absenkung": ("19:00", "07:00", 3.0)},
}

# Zeitzone: Mitteleuropa (MEZ/MESZ) wird in main.py berechnet.
