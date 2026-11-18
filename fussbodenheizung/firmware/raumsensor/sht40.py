# -*- coding: utf-8 -*-
"""Minimaler Treiber für Sensirion SHT40 (I2C-Adresse 0x44).

Befehle und Umrechnung nach Sensirion SHT4x-Datenblatt:
  0xFD = Messung hohe Wiederholgenauigkeit (max. ca. 8,3 ms)
  T  = -45 + 175 * S_T  / 65535   [°C]
  RH =  -6 + 125 * S_RH / 65535   [%]
  CRC-8: Polynom 0x31, Startwert 0xFF
"""
import time

ADRESSE = 0x44
CMD_MESSUNG_HOCH = 0xFD
CMD_SOFT_RESET = 0x94


def _crc8(daten):
    crc = 0xFF
    for b in daten:
        crc ^= b
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ 0x31) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc


class SHT40:
    def __init__(self, i2c, adresse=ADRESSE):
        self.i2c = i2c
        self.adr = adresse

    def reset(self):
        self.i2c.writeto(self.adr, bytes([CMD_SOFT_RESET]))
        time.sleep_ms(2)

    def messen(self):
        """Gibt (temperatur_c, rel_feuchte_prozent) zurück."""
        self.i2c.writeto(self.adr, bytes([CMD_MESSUNG_HOCH]))
        time.sleep_ms(10)
        d = self.i2c.readfrom(self.adr, 6)
        if _crc8(d[0:2]) != d[2] or _crc8(d[3:5]) != d[5]:
            raise ValueError("SHT40 CRC-Fehler")
        st = (d[0] << 8) | d[1]
        srh = (d[3] << 8) | d[4]
        t = -45.0 + 175.0 * st / 65535.0
        rh = -6.0 + 125.0 * srh / 65535.0
        rh = min(max(rh, 0.0), 100.0)
        return t, rh
