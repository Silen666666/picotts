"""Standard-Suchregion: Schloß Holte-Stukenbrock und Umgebung.

Quellen für die Vorgaben (Stand der Recherche: 09.11.2026):
- PLZ 33758 für das gesamte Stadtgebiet und die Ortsteile Liemke, Schloß Holte,
  Sende, Stukenbrock, Stukenbrock-Senne:
  https://onlinestreet.de/plz/Schlo%C3%9F+Holte-Stukenbrock.html
  https://www.suche-postleitzahl.org/?p=21440
- Orte in der Nähe mit Entfernungsangaben (Luftlinie, keine amtlichen
  Gemeindegrenzen): https://www.postleitzahl.org/nordrhein_westfalen/schlo%C3%9F_holte-stukenbrock/

Alle Werte lassen sich im Webinterface unter "Einstellungen" ändern.
"""

# (Name, Entfernung in km laut Quelle oder None, standardmäßig aktiv)
DEFAULT_PLACES = [
    ("Schloß Holte-Stukenbrock", 0.0, True),
    ("Oerlinghausen", 6.26, True),
    ("Verl", 8.44, True),
    ("Augustdorf", 8.6, True),
    ("Hövelhof", 9.2, True),
    ("Delbrück", 15.78, True),
]

# Ortsteile von Schloß Holte-Stukenbrock (alle PLZ 33758). Treffer, die nur
# einen Ortsteil nennen, werden der Stadt zugeordnet.
DISTRICTS = {
    "Schloß Holte-Stukenbrock": [
        "Schloß Holte", "Schloss Holte", "Stukenbrock", "Stukenbrock-Senne",
        "Liemke", "Sende", "33758",
    ],
}

# Portale, die per "site:"-Filter durchsucht werden. Es wird nur die
# Websuche verwendet, die Portale selbst werden nicht automatisiert abgerufen.
DEFAULT_DOMAINS = [
    "immobilienscout24.de",
    "immowelt.de",
    "kleinanzeigen.de",
    "wg-gesucht.de",
]
