# -*- coding: utf-8 -*-
"""Tests des Regelkerns (CPython).  Aufruf:  python3 -m unittest discover -s tests"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "firmware", "zentrale"))
from regler import Regler, Zone, sollwert_aktuell  # noqa: E402


class TestSollwert(unittest.TestCase):
    def test_absenkung_ueber_mitternacht(self):
        cfg = {"soll": 21.0, "absenkung": ("22:00", "06:00", 2.0)}
        self.assertEqual(sollwert_aktuell(cfg, 23 * 60), 19.0)
        self.assertEqual(sollwert_aktuell(cfg, 3 * 60), 19.0)
        self.assertEqual(sollwert_aktuell(cfg, 12 * 60), 21.0)

    def test_ohne_uhrzeit_keine_absenkung(self):
        cfg = {"soll": 21.0, "absenkung": ("22:00", "06:00", 2.0)}
        self.assertEqual(sollwert_aktuell(cfg, None), 21.0)


class TestZone(unittest.TestCase):
    def test_fuehlerausfall_ersatzstellgrad(self):
        z = Zone("x", 0)
        z.soll = 21.0
        z.schritt(0.0)
        self.assertEqual(z.status, "FUEHLER_AUSFALL")
        self.assertAlmostEqual(z.stellgrad, 0.30)

    def test_frostschutz(self):
        z = Zone("x", 0)
        z.soll = 21.0
        z.messwert(5.0, 0.0)
        self.assertTrue(z.schritt(0.0))
        self.assertEqual(z.stellgrad, 1.0)
        self.assertEqual(z.status, "FROSTSCHUTZ")

    def test_zu_warm_ventil_zu(self):
        z = Zone("x", 0)
        z.soll = 20.0
        z.messwert(23.0, 0.0)
        self.assertFalse(z.schritt(0.0))
        self.assertEqual(z.stellgrad, 0.0)

    def test_pwm_anteil(self):
        z = Zone("x", 0, kp=0.5, tn_s=1e12)   # praktisch reiner P-Regler
        z.soll = 21.0
        z.messwert(20.0, 0.0)                  # 1 K zu kalt -> 50 %
        ein = 0
        for s in range(0, 1200, 10):
            z.messwert(20.0, float(s))
            ein += z.schritt(float(s))
        self.assertAlmostEqual(ein * 10 / 1200.0, 0.5, delta=0.02)

    def test_ventilschutz(self):
        z = Zone("x", 0, ventilschutz_intervall_s=3600, ventilschutz_dauer_s=600)
        z.soll = 18.0
        offen = []
        for s in range(0, 7200, 10):
            z.messwert(25.0, float(s))
            offen.append(z.schritt(float(s)))
        self.assertTrue(any(offen))
        self.assertLessEqual(sum(offen) * 10, 600 + 10)


class TestRegler(unittest.TestCase):
    def test_versatz_und_anforderung(self):
        zonen = {"a": {"ausgang": 0, "soll": 25.0}, "b": {"ausgang": 1, "soll": 25.0}}
        r = Regler(zonen, 8)
        r.messwert("a", 20.0, 0.0)
        r.messwert("b", 20.0, 0.0)
        aus, anf = r.schritt(0.0)
        self.assertTrue(aus[0])
        self.assertFalse(aus[1])      # Zone b startet versetzt
        self.assertTrue(anf)
        aus, _ = r.schritt(600.0)
        self.assertTrue(aus[1])


if __name__ == "__main__":
    unittest.main()
