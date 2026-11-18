# -*- coding: utf-8 -*-
"""Konfiguration eines Raumsensors (ESP32-C3 + SHT40). Für jeden Raum anpassen."""

# MAC-Adresse der Zentrale (wird beim Start der Zentrale ausgegeben)
ZENTRALE_MAC = "00:00:00:00:00:00"

# I2C-Pins zum SHT40 (ESP32-C3-DevKitM-1: GPIO8 = RGB-LED, GPIO9 = BOOT -> meiden)
I2C_SDA = 4
I2C_SCL = 5

# Kalibrier-Offset in Kelvin (Vergleich mit Referenzthermometer, siehe README)
OFFSET_K = 0.0

# Sendeintervall. Netzbetrieb (USB-Netzteil): 60 s. Batteriebetrieb: 300 s.
INTERVALL_S = 60

# Deep-Sleep zwischen den Messungen (True = stromsparend, empfohlen; hält zudem
# die Eigenerwärmung des ESP32 vom Fühler fern)
DEEPSLEEP = True
