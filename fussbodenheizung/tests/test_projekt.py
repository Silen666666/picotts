# -*- coding: utf-8 -*-
"""
Konsistenz- und Logiktests (Python 3, benötigt PyYAML).
Aufruf:  python3 -m unittest discover -s tests

Prüft u.a.: Jede Zone der Zentrale hat einen Raumsensor, einen UDP-Provider
und einen Eintrag im Home-Assistant-Generator; keine doppelten GPIOs/Bits;
nur erlaubte ESP32-S3-GPIOs; Sicherheitslogik der Simulation.
"""
import math
import os
import sys
import unittest

import yaml

BASIS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, os.path.join(BASIS, "tools"))

import erzeuge_homeassistant as ha  # noqa: E402
from simulation import EsphomePID, sicherheitslogik  # noqa: E402


class _Lader(yaml.SafeLoader):
    pass


def _tag(loader, suffix, node):
    if isinstance(node, yaml.MappingNode):
        return {"_tag": suffix, **loader.construct_mapping(node, deep=True)}
    return {"_tag": suffix, "wert": loader.construct_scalar(node)}


_Lader.add_multi_constructor("!", _tag)


def lade(pfad):
    with open(os.path.join(BASIS, pfad), encoding="utf-8") as f:
        return yaml.load(f, Loader=_Lader)


# ESP32-S3 N16R8: Strapping 0,3,45,46 · USB 19,20 · Flash 26-32 · PSRAM 33-37 ·
# UART0 43,44 · RGB-LED 48  -> nicht als Relaisausgang
VERBOTEN_S3 = {0, 3, 45, 46, 19, 20, 43, 44, 48} | set(range(26, 38))


class TestKonsistenz(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.zentrale = lade("esphome/fbh-zentrale.yaml")
        cls.zonen = {k: v["vars"] for k, v in cls.zentrale["packages"].items()}

    def test_gpio_und_bits_eindeutig(self):
        gpios = [v["gpio"] for v in self.zonen.values()] + ["GPIO8", "GPIO9"]
        self.assertEqual(len(gpios), len(set(gpios)))
        bits = [v["bit"] for v in self.zonen.values()]
        self.assertEqual(len(bits), len(set(bits)))
        self.assertTrue(all(0 <= b < 32 for b in bits))

    def test_gpio_erlaubt_esp32s3(self):
        for v in self.zonen.values():
            nr = int(v["gpio"].replace("GPIO", ""))
            self.assertNotIn(nr, VERBOTEN_S3, v)

    def test_jede_zone_hat_sensor_und_provider(self):
        provider = {p["name"] for p in self.zentrale["packet_transport"][0]["providers"]}
        for z in self.zonen:
            self.assertEqual(self.zonen[z]["zone"], z)
            self.assertIn("fbh-sensor-%s" % z, provider)
            sensor = lade("esphome/fbh-sensor-%s.yaml" % z)
            self.assertEqual(sensor["substitutions"]["name"], "fbh-sensor-%s" % z)

    def test_homeassistant_zonen_passen(self):
        self.assertEqual([z[0] for z in ha.ZONEN], list(self.zonen))

    def test_homeassistant_paket_erzeugbar(self):
        p = ha.paket()
        for z, *_ in ha.ZONEN:
            self.assertIn("fbh_%s" % z, p["schedule"])
            self.assertIn("fbh_%s_komfort" % z, p["input_number"])
            self.assertIn("fbh_%s_absenk" % z, p["input_number"])

    def test_entity_ids_mit_umlauten(self):
        self.assertEqual(ha.slug("FBH Zentrale Küche"), "fbh_zentrale_kuche")
        self.assertEqual(ha.slug("FBH Zentrale Wärmeanforderung"), "fbh_zentrale_warmeanforderung")
        self.assertEqual(ha.klima("buero"), "climate.fbh_zentrale_buro")

    def test_display_variable_passt_zu_ha(self):
        # Raumsensoren mit Display: ha_raum muss dem HA-Slug des Raumnamens entsprechen
        for z, name, *_ in ha.ZONEN:
            sensor = lade("esphome/fbh-sensor-%s.yaml" % z)
            bed = sensor["packages"].get("bedienung")
            if bed:
                self.assertEqual(bed["vars"]["ha_raum"], ha.slug(name))

    def test_statustext_identisch(self):
        # Automation "Fühlerausfall melden" muss exakt den Text aus zone.yaml treffen
        with open(os.path.join(BASIS, "esphome/common/zone.yaml"), encoding="utf-8") as f:
            zone = f.read()
        text = ha.paket()["automation"][1]["triggers"][0]["to"]
        self.assertIn('"%s"' % text, zone)


class TestRegellogik(unittest.TestCase):
    def test_fuehlerausfall(self):
        self.assertEqual(sicherheitslogik(0.7, math.nan), (0.30, "Fühlerausfall"))

    def test_frostschutz_auch_wenn_aus(self):
        self.assertEqual(sicherheitslogik(0.0, 6.0, aus=True)[0], 1.0)

    def test_aus(self):
        self.assertEqual(sicherheitslogik(0.8, 20.0, aus=True)[0], 0.0)

    def test_mindestanteil(self):
        self.assertEqual(sicherheitslogik(0.05, 20.0)[0], 0.0)
        self.assertEqual(sicherheitslogik(0.95, 20.0)[0], 1.0)
        self.assertAlmostEqual(sicherheitslogik(0.4, 20.0)[0], 0.4)

    def test_pid_integral_begrenzt(self):
        pid = EsphomePID(kp=0.5, ki=0.5 / 7200)
        for i in range(10000):
            y = pid.update(25.0, 15.0, i * 60.0)
        self.assertEqual(pid.integral, 1.0)
        self.assertEqual(y, 1.0)
        pid.update(15.0, 25.0, 10000 * 60.0)
        self.assertGreaterEqual(pid.integral, 0.0)


if __name__ == "__main__":
    unittest.main()
