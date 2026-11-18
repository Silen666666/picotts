# -*- coding: utf-8 -*-
"""
Raumsensor (MicroPython, ESP32-C3 + SHT40).

Misst Temperatur/Feuchte, sendet per ESP-NOW an die Zentrale und schläft dann.
Kanalsuche: Ist die Zentrale mit dem WLAN-Router verbunden, funkt sie auf dessen
Kanal. Der Sensor merkt sich den zuletzt erfolgreichen Kanal im RTC-Speicher und
probiert bei fehlender Empfangsbestätigung (ACK) die Kanäle 1..13 durch.

Dateien auf dem ESP32-C3:  main.py, config.py, sht40.py
"""
import time
import json
import machine
import network
import espnow

import config
from sht40 import SHT40


def mac_bytes(txt):
    return bytes(int(x, 16) for x in txt.split(":"))


def kanal_laden():
    try:
        m = machine.RTC().memory()
        if m and 1 <= m[0] <= 13:
            return m[0]
    except Exception:
        pass
    return 1


def kanal_speichern(k):
    try:
        machine.RTC().memory(bytes([k]))
    except Exception:
        pass


def messen():
    i2c = machine.I2C(0, sda=machine.Pin(config.I2C_SDA),
                      scl=machine.Pin(config.I2C_SCL), freq=100000)
    s = SHT40(i2c)
    werte = []
    for _ in range(3):
        try:
            werte.append(s.messen())
        except Exception as ex:
            print("Messfehler:", ex)
        time.sleep_ms(50)
    if not werte:
        return None, None
    t = sum(w[0] for w in werte) / len(werte) + config.OFFSET_K
    rh = sum(w[1] for w in werte) / len(werte)
    return t, rh


def senden(nutzdaten):
    sta = network.WLAN(network.STA_IF)
    sta.active(True)
    sta.disconnect()
    print("Sensor MAC:", ":".join("%02x" % b for b in sta.config("mac")))

    en = espnow.ESPNow()
    en.active(True)
    peer = mac_bytes(config.ZENTRALE_MAC)
    try:
        en.add_peer(peer)
    except OSError:
        pass  # bereits vorhanden

    start = kanal_laden()
    kanaele = [start] + [k for k in range(1, 14) if k != start]
    for k in kanaele:
        try:
            sta.config(channel=k)
        except Exception:
            pass
        try:
            if en.send(peer, nutzdaten, True):   # True = auf ACK warten
                kanal_speichern(k)
                print("gesendet auf Kanal", k)
                en.active(False)
                return True
        except OSError as ex:
            print("Sendefehler:", ex)
    en.active(False)
    print("Zentrale nicht erreichbar")
    return False


def einmal():
    t, rh = messen()
    if t is None:
        return
    daten = json.dumps({"t": round(t, 2), "h": round(rh, 1)})
    print(daten)
    senden(daten)


if config.DEEPSLEEP:
    einmal()
    machine.deepsleep(config.INTERVALL_S * 1000)
else:
    while True:
        einmal()
        time.sleep(config.INTERVALL_S)
