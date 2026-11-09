# Wohnungssuchagent – Schloß Holte-Stukenbrock & Umgebung

Ein Python-Programm, das regelmäßig per Websuche nach Wohnungsangeboten in
Schloß Holte-Stukenbrock und Umgebung sucht. Es bewertet die Treffer nach deinen
Kriterien, speichert sie und zeigt sie in einem Webinterface im Browser an.

## Funktionen

- **Websuche** über das Paket [`ddgs`](https://pypi.org/project/ddgs/) (Metasuche,
  kein API-Schlüssel nötig) oder optional über eine eigene
  [SearXNG](https://docs.searxng.org/dev/search_api.html)-Instanz.
- **Portale** werden per `site:`-Filter durchsucht (Vorgabe: immobilienscout24.de,
  immowelt.de, kleinanzeigen.de, wg-gesucht.de), dazu kommt eine freie Suche ohne Filter.
  Die Portale selbst werden nicht automatisiert abgerufen (Scraping).
- **Auswertung** von Preis, Zimmerzahl, Wohnfläche und Ort aus Titel und Kurztext der Treffer,
  Abgleich mit Höchstpreis, Mindestzimmern, Mindestfläche, Ausschluss- und Pluswörtern,
  Bewertung von 0 bis 100.
- **Duplikaterkennung** über bereinigte URLs, Status je Treffer (neu, gesehen, Favorit,
  ausgeblendet) und eigene Notizen.
- **Agent-Modus**: automatische Suche im einstellbaren Intervall (mindestens 15 Minuten),
  Protokoll aller Suchläufe mit Fehlern.
- **Optional E-Mail** bei neuen passenden Treffern (siehe unten).

## Start

Voraussetzung: Python 3.10 oder neuer.

```bash
# Linux / macOS
./start.sh
# Windows (Doppelklick oder in der Eingabeaufforderung)
start.bat
```

Danach im Browser **http://127.0.0.1:5000** öffnen. Manuell geht es auch so:

```bash
python -m venv .venv
.venv/bin/pip install -r requirements.txt     # Windows: .venv\Scripts\pip ...
.venv/bin/python app.py --port 5000
```

Optionen: `--host 0.0.0.0` (im Heimnetz erreichbar, **ohne Passwortschutz**),
`--port`, `--db pfad/zur/datei.db`.

## Bedienung

1. Unter **Einstellungen** Miete oder Kauf, Höchstpreis, Zimmer, Fläche und Orte festlegen.
2. Unter **Treffer** auf **„Jetzt suchen“** klicken. Ein Lauf mit 6 Orten und 4 Portalen
   besteht aus 30 Anfragen und dauert wegen der Pausen zwischen den Anfragen einige Minuten.
3. Wer regelmäßig suchen lassen will, setzt unter *Einstellungen → Agent* den Haken bei
   „automatisch regelmäßig suchen“. Die Suche läuft nur, solange das Programm geöffnet ist.

## Region (Vorgabe)

| Ort | Entfernung* |
|---|---|
| Schloß Holte-Stukenbrock (PLZ 33758, Ortsteile Liemke, Schloß Holte, Sende, Stukenbrock, Stukenbrock-Senne) | – |
| Oerlinghausen | 6,26 km |
| Verl | 8,44 km |
| Augustdorf | 8,6 km |
| Hövelhof | 9,2 km |
| Delbrück | 15,78 km |

\* Luftlinie laut [postleitzahl.org](https://www.postleitzahl.org/nordrhein_westfalen/schlo%C3%9F_holte-stukenbrock/).
Das ist keine amtliche Nachbarschaftsliste. PLZ und Ortsteile stammen von
[onlinestreet.de](https://onlinestreet.de/plz/Schlo%C3%9F+Holte-Stukenbrock.html) und
[suche-postleitzahl.org](https://www.suche-postleitzahl.org/?p=21440). Die Quellen nennen
nicht alle dieselben Ortsteile, „Stukenbrock-Senne“ führt nur onlinestreet.de auf.
Weitere Orte (z. B. Bielefeld-Sennestadt, Gütersloh, Paderborn) lassen sich in den
Einstellungen ergänzen.

## E-Mail-Benachrichtigung (optional)

Vor dem Start diese Umgebungsvariablen setzen:

```
SMTP_HOST=smtp.example.de  SMTP_PORT=587  SMTP_USER=...  SMTP_PASS=...  MAIL_TO=du@example.de
```

Nach jedem Suchlauf kommt dann eine Mail mit den neuen passenden Treffern (STARTTLS).

## Grenzen – bitte beachten

- Preis, Zimmer und Fläche stammen aus den **Kurztexten der Suchmaschine**. Sie können
  fehlen, falsch erkannt oder veraltet sein. Maßgeblich ist immer das Originalinserat.
- Suchmaschinen erfassen neue Inserate verzögert und nicht vollständig. Das Programm
  **ergänzt** die Suchaufträge der Portale (z. B. E-Mail-Alarme bei ImmoScout24, Immowelt,
  Kleinanzeigen), ersetzt sie aber nicht.
- Kostenlose Metasuchen begrenzen bei zu vielen Anfragen die Rate. Dann helfen eine längere
  Pause, ein größeres Intervall oder eine eigene SearXNG-Instanz.
- Gefundene Übersichtsseiten (z. B. „34 Wohnungen in Hövelhof“) werden mit dem Hinweis
  „Übersicht“ angezeigt und abgewertet. Der Filter „nur Einzelinserate“ blendet sie aus.

## Tests

```bash
.venv/bin/pip install pytest
.venv/bin/python -m pytest -q
```

Die Tests laufen ohne Internet und verwenden erfundene Beispieltreffer.

## Dateien

| Datei | Inhalt |
|---|---|
| `app.py` | Webinterface (Flask) und Startpunkt |
| `agent.py` | Suchagent: Anfragen, Websuche, Zeitplan, E-Mail |
| `extractor.py` | Erkennung von Preis, Zimmern, Fläche, Ort, Bewertung |
| `storage.py` | SQLite-Datenbank (`wohnungen.db`) |
| `region.py` | Vorgaben für Orte, Ortsteile und Portale mit Quellen |
