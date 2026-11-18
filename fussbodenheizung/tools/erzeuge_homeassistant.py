# -*- coding: utf-8 -*-
"""
Erzeugt die Home-Assistant-Dateien für die Fußbodenheizung aus der Zonenliste:

  homeassistant/packages/fussbodenheizung.yaml   Helfer, Zeitpläne, Automationen
  homeassistant/dashboard_fussbodenheizung.yaml  Dashboard-Ansicht (YAML)

Aufruf:  python3 tools/erzeuge_homeassistant.py
Benötigt: PyYAML (pip install pyyaml)

Die Zonen müssen zu esphome/fbh-zentrale.yaml (packages) passen.
"""
import os

import yaml

HIER = os.path.dirname(os.path.abspath(__file__))
ZIEL = os.path.join(HIER, "..", "homeassistant")

# zone, Anzeigename, Komfort °C, Absenk °C, Komfortzeiten Mo–Fr, Komfortzeiten Sa/So
ZONEN = [
    ("wohnen",   "Wohnen",   21.0, 19.0, [("05:30", "22:30")], [("07:00", "23:00")]),
    ("kueche",   "Küche",    20.5, 18.5, [("05:30", "22:30")], [("07:00", "23:00")]),
    ("bad",      "Bad",      23.0, 21.0, [("05:00", "08:00"), ("18:00", "23:00")],
                                         [("07:00", "23:00")]),
    ("schlafen", "Schlafen", 18.0, 17.0, [("05:30", "22:00")], [("07:00", "22:00")]),
    ("kind",     "Kind",     20.0, 18.0, [("06:00", "21:00")], [("07:00", "21:00")]),
    ("buero",    "Büro",     20.5, 17.5, [("07:00", "19:00")], []),
]
WERKTAGE = ["monday", "tuesday", "wednesday", "thursday", "friday"]
WOCHENENDE = ["saturday", "sunday"]

def slug(text):
    """Wie Home Assistant Entity-IDs bildet (homeassistant.util.slugify):
    Umlaute -> Grundvokal (ü -> u), ß -> ss, alles andere -> '_'."""
    t = text.lower()
    for a, b in (("ä", "a"), ("ö", "o"), ("ü", "u"), ("ß", "ss")):
        t = t.replace(a, b)
    out, unter = [], False
    for c in t:
        if c.isascii() and c.isalnum():
            out.append(c)
            unter = False
        elif not unter:
            out.append("_")
            unter = True
    return "".join(out).strip("_")


NAMEN = {z[0]: z[1] for z in ZONEN}


# Entity-IDs, wie Home Assistant sie aus den ESPHome-Namen bildet:
# Gerät "FBH Zentrale" bzw. "Raumsensor <Name>" + Entitätsname.
def klima(z):
    return "climate." + slug("FBH Zentrale " + NAMEN[z])


def status(z):
    return "sensor." + slug("FBH Zentrale Status " + NAMEN[z])


def stellgrad(z):
    return "sensor." + slug("FBH Zentrale Stellgrad " + NAMEN[z])


def feuchte(z):
    return "sensor." + slug("Raumsensor %s Luftfeuchte" % NAMEN[z])


def zeiten(liste):
    return [{"from": a + ":00", "to": b + ":00"} for a, b in liste]


def paket():
    p = {"input_number": {}, "input_boolean": {}, "schedule": {}, "automation": []}
    p["input_boolean"]["fbh_zeitprogramm"] = {
        "name": "FBH Zeitprogramm aktiv", "icon": "mdi:calendar-clock", "initial": True}
    p["input_boolean"]["fbh_abwesend"] = {
        "name": "FBH Abwesend (alle Räume Absenkung)", "icon": "mdi:home-export-outline"}

    for z, name, komfort, absenk, wt, we in ZONEN:
        for art, wert, icon in (("komfort", komfort, "mdi:sofa"),
                                ("absenk", absenk, "mdi:weather-night")):
            p["input_number"]["fbh_%s_%s" % (z, art)] = {
                "name": "%s %s" % (name, "Komfort" if art == "komfort" else "Absenkung"),
                "min": 5, "max": 28, "step": 0.5, "initial": wert,
                "unit_of_measurement": "°C", "mode": "box", "icon": icon}
        plan = {"name": "FBH %s Komfortzeiten" % name, "icon": "mdi:calendar-week"}
        for tag in WERKTAGE:
            plan[tag] = zeiten(wt)
        for tag in WOCHENENDE:
            plan[tag] = zeiten(we)
        p["schedule"]["fbh_%s" % z] = plan

    zonen = [z[0] for z in ZONEN]
    p["automation"].append({
        "id": "fbh_zeitprogramm_anwenden",
        "alias": "FBH – Zeitprogramm auf Sollwerte anwenden",
        "description": "Setzt je Raum Komfort- oder Absenktemperatur an der Zentrale "
                       "(Zeitplan, Abwesenheit). Manuelle Änderungen gelten bis zum "
                       "nächsten Schaltpunkt.",
        "mode": "queued",
        "triggers": [
            {"trigger": "homeassistant", "event": "start"},
            {"trigger": "state", "entity_id": ["schedule.fbh_%s" % z for z in zonen]},
            {"trigger": "state",
             "entity_id": ["input_number.fbh_%s_%s" % (z, a) for z in zonen
                           for a in ("komfort", "absenk")]
             + ["input_boolean.fbh_abwesend", "input_boolean.fbh_zeitprogramm"]},
        ],
        "conditions": [{"condition": "state", "entity_id": "input_boolean.fbh_zeitprogramm",
                        "state": "on"}],
        "actions": [{
            "repeat": {
                "for_each": [{"zone": z, "klima": klima(z)} for z in zonen],
                "sequence": [
                    {"variables": {
                        "komfort": "{{ is_state('schedule.fbh_' ~ repeat.item.zone, 'on') and "
                                   "not is_state('input_boolean.fbh_abwesend', 'on') }}",
                        "soll": "{{ states('input_number.fbh_' ~ repeat.item.zone ~ "
                                "('_komfort' if komfort else '_absenk')) | float(20) }}",
                        "ziel": "{{ repeat.item.klima }}",
                    }},
                    {"condition": "template",
                     "value_template": "{{ states(ziel) not in ['unavailable', 'unknown'] }}"},
                    {"action": "climate.set_temperature",
                     "target": {"entity_id": "{{ ziel }}"},
                     "data": {"temperature": "{{ soll }}"}},
                ],
            }
        }],
    })
    p["automation"].append({
        "id": "fbh_fuehlerausfall_melden",
        "alias": "FBH – Fühlerausfall melden",
        "mode": "parallel",
        "triggers": [{"trigger": "state", "entity_id": [status(z) for z in zonen],
                      "to": "Fühlerausfall (Ersatzbetrieb 30 %)", "for": "00:05:00"}],
        "actions": [{"action": "persistent_notification.create",
                     "data": {"title": "Fußbodenheizung",
                              "notification_id": "fbh_{{ trigger.entity_id }}",
                              "message": "{{ state_attr(trigger.entity_id, 'friendly_name') }}: "
                                         "kein Messwert vom Raumsensor seit 15 min – Ventil "
                                         "läuft mit 30 % Ersatzbetrieb."}}],
    })
    p["automation"].append({
        "id": "fbh_zentrale_offline_melden",
        "alias": "FBH – Zentrale nicht erreichbar",
        "mode": "single",
        "triggers": [{"trigger": "state", "entity_id": klima(zonen[0]),
                      "to": "unavailable", "for": "00:10:00"}],
        "actions": [{"action": "persistent_notification.create",
                     "data": {"title": "Fußbodenheizung",
                              "notification_id": "fbh_zentrale",
                              "message": "Die FBH-Zentrale ist seit 10 min nicht erreichbar. "
                                         "Sie regelt lokal mit den zuletzt gespeicherten "
                                         "Sollwerten weiter; Zeitprogramm wirkt erst wieder "
                                         "nach Verbindung."}}],
    })
    return p


def dashboard():
    karten = [{
        "type": "entities", "title": "Fußbodenheizung",
        "entities": ["input_boolean.fbh_zeitprogramm", "input_boolean.fbh_abwesend",
                     "binary_sensor." + slug("FBH Zentrale Wärmeanforderung")]}]
    for z, name, *_ in ZONEN:
        karten.append({"type": "vertical-stack", "cards": [
            {"type": "thermostat", "entity": klima(z), "name": name},
            {"type": "entities", "entities": [
                {"entity": stellgrad(z), "name": "Ventil (Stellgrad)"},
                {"entity": status(z), "name": "Status"},
                {"entity": feuchte(z), "name": "Luftfeuchte"},
                {"entity": "input_number.fbh_%s_komfort" % z, "name": "Komfort"},
                {"entity": "input_number.fbh_%s_absenk" % z, "name": "Absenkung"},
                {"entity": "schedule.fbh_%s" % z, "name": "Komfortzeit aktiv"},
            ]},
        ]})
    karten.append({"type": "history-graph", "title": "Raumtemperaturen (24 h)",
                   "hours_to_show": 24,
                   "entities": [klima(z[0]) for z in ZONEN]})
    return {"title": "Fußbodenheizung", "views": [
        {"title": "Heizung", "path": "heizung", "icon": "mdi:radiator", "cards": karten}]}


class _Dumper(yaml.SafeDumper):
    pass


def _str(dumper, data):
    return dumper.represent_scalar("tag:yaml.org,2002:str", data,
                                   style='"' if ("{" in data or ":" in data) else None)


_Dumper.add_representer(str, _str)


def schreibe(pfad, kopf, daten):
    os.makedirs(os.path.dirname(pfad), exist_ok=True)
    with open(pfad, "w", encoding="utf-8") as f:
        f.write(kopf)
        yaml.dump(daten, f, Dumper=_Dumper, allow_unicode=True, sort_keys=False, width=100)
    print("geschrieben:", os.path.normpath(pfad))


if __name__ == "__main__":
    schreibe(os.path.join(ZIEL, "packages", "fussbodenheizung.yaml"),
             "# Fußbodenheizung – Home-Assistant-Paket (erzeugt mit tools/erzeuge_homeassistant.py)\n"
             "# Einbinden in configuration.yaml:\n"
             "#   homeassistant:\n"
             "#     packages: !include_dir_named packages\n"
             "# und diese Datei nach /config/packages/ kopieren.\n\n",
             paket())
    schreibe(os.path.join(ZIEL, "dashboard_fussbodenheizung.yaml"),
             "# Dashboard (erzeugt mit tools/erzeuge_homeassistant.py)\n"
             "# HA: Einstellungen > Dashboards > Dashboard hinzufügen > (neu) > ⋮ > Raw-Konfigurationseditor\n"
             "#     -> Inhalt einfügen.\n\n",
             dashboard())
