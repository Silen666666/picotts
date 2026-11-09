"""Tests ohne Netzzugriff: Websuche wird durch feste Beispieltreffer ersetzt.

Aufruf (im Ordner wohnungsagent):  python -m pytest -q
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from agent import Agent, build_queries  # noqa: E402
from app import create_app  # noqa: E402
from extractor import (Criteria, canonical_url, detect_place, evaluate,  # noqa: E402
                       extract_area, extract_price, extract_rooms)
from region import DISTRICTS  # noqa: E402
from storage import Storage  # noqa: E402

PLACES = ["Schloß Holte-Stukenbrock", "Verl", "Oerlinghausen", "Hövelhof"]

# Erfundene Beispieltreffer im Format der Suchmaschine (nur für Tests).
FAKE_RESULTS = [
    {"title": "Schöne 3-Zimmer-Wohnung mit Balkon in Schloß Holte-Stukenbrock",
     "url": "https://www.immobilienscout24.de/expose/123456789?utm_source=x",
     "body": "Kaltmiete 780 €, 82 m², 3 Zimmer, Stellplatz vorhanden."},
    {"title": "2 Zimmer Wohnung Liemke",
     "url": "https://www.kleinanzeigen.de/s-anzeige/2-zimmer-wohnung/2999999-203-1234",
     "body": "650,- € kalt, ca. 58 qm, ab sofort frei"},
    {"title": "Luxus-Penthouse in Verl",
     "url": "https://www.immowelt.de/expose/abc123",
     "body": "Miete 1.950 € | 140 m² | 4 Zi."},
    {"title": "Wohnungen mieten in Hövelhof – 34 Angebote",
     "url": "https://www.immowelt.de/suche/hoevelhof/wohnungen/mieten",
     "body": "Finden Sie Ihre Mietwohnung in Hövelhof"},
    {"title": "Wohnung in Berlin-Mitte", "url": "https://example.org/berlin",
     "body": "2 Zimmer, 600 €, 55 m²"},
]


def test_extract_numbers():
    assert extract_price("Kaltmiete 780 €, 82 m²") == 780
    assert extract_price("Miete 1.950 € | 140 m²") == 1950
    assert extract_price("650,- € kalt") == 650
    assert extract_price("Kaufpreis 249.000 €", mode="kauf") == 249000
    assert extract_price("Baujahr 1995, 3 Zimmer") is None
    assert extract_rooms("Schöne 3-Zimmer-Wohnung") == 3
    assert extract_rooms("2,5 Zi. Wohnung") == 2.5
    assert extract_area("ca. 58 qm") == 58
    assert extract_area("82,5 m² Wohnfläche") == 82.5


def test_detect_place_and_districts():
    assert detect_place("Wohnung in Liemke", PLACES, DISTRICTS) == "Schloß Holte-Stukenbrock"
    assert detect_place("Schloss Holte-Stukenbrock, 3 Zi", PLACES, DISTRICTS) == \
        "Schloß Holte-Stukenbrock"
    assert detect_place("Wohnung Hoevelhof", PLACES, DISTRICTS) is None  # "oe" != "ö"
    assert detect_place("Wohnung Hövelhof Zentrum", PLACES, DISTRICTS) == "Hövelhof"
    assert detect_place("Wohnung in Verlängerung", PLACES, DISTRICTS) is None
    assert detect_place("Wohnung in Berlin", PLACES, DISTRICTS) is None


def test_canonical_url_strips_tracking():
    assert canonical_url("https://www.Immowelt.de/expose/abc/?utm_source=x#top") == \
        "https://immowelt.de/expose/abc"


def test_evaluate_criteria():
    crit = Criteria(max_price=900, min_rooms=2, min_area=50, exclude_words=["Tausch"])
    ok = evaluate(**{k: FAKE_RESULTS[0][k] for k in ("title", "url", "body")},
                  criteria=crit, places=PLACES, districts=DISTRICTS)
    assert ok.matches and ok.is_listing and ok.score >= 80
    too_expensive = evaluate(FAKE_RESULTS[2]["title"], FAKE_RESULTS[2]["body"],
                             FAKE_RESULTS[2]["url"], crit, PLACES, DISTRICTS)
    assert not too_expensive.matches
    berlin = evaluate(FAKE_RESULTS[4]["title"], FAKE_RESULTS[4]["body"],
                      FAKE_RESULTS[4]["url"], crit, PLACES, DISTRICTS)
    assert not berlin.matches
    tausch = evaluate("Wohnungstausch 3 Zimmer Verl", "700 €, 70 m²", "https://x.de/a",
                      crit, PLACES, DISTRICTS)
    assert not tausch.matches


def test_build_queries():
    qs = build_queries({"mode": "miete", "places": ["Verl"], "domains": ["immowelt.de"],
                        "general_query": True, "extra_terms": "Balkon"})
    assert qs == ['Wohnung mieten "Verl" Balkon site:immowelt.de',
                  'Wohnung mieten "Verl" Balkon']


def test_agent_run_and_dedup(tmp_path):
    storage = Storage(str(tmp_path / "t.db"))
    storage.save_settings({"places": PLACES, "domains": ["immowelt.de"]})
    agent = Agent(storage, search_func=lambda q: FAKE_RESULTS)
    first = agent.run_once()
    assert first["new"] == len(FAKE_RESULTS)          # Duplikate je Lauf zusammengeführt
    second = agent.run_once()
    assert second["new"] == 0
    matching = storage.list_listings(only_matches=True)
    # Verl (zu teuer) und Berlin (außerhalb) fallen raus; die Hövelhof-Übersichtsseite
    # bleibt sichtbar, wird aber abgewertet und ist kein Einzelinserat.
    assert {r["place"] for r in matching} == {"Schloß Holte-Stukenbrock", "Hövelhof"}
    listings_only = storage.list_listings(only_matches=True, only_listings=True)
    assert {r["place"] for r in listings_only} == {"Schloß Holte-Stukenbrock"}
    assert matching[-1]["place"] == "Hövelhof"           # niedrigste Bewertung
    # Neubewertung nach geänderten Kriterien
    storage.save_settings({"max_price": 2500, "min_area": None, "min_rooms": None})
    agent.reevaluate()
    assert any(r["place"] == "Verl" for r in storage.list_listings(only_matches=True))


def test_web_interface(tmp_path):
    app = create_app(str(tmp_path / "w.db"), search_func=lambda q: FAKE_RESULTS,
                     start_agent=False)
    app.config["agent"].run_once()
    client = app.test_client()
    page = client.get("/").get_data(as_text=True)
    assert "3-Zimmer-Wohnung mit Balkon" in page
    assert "Berlin-Mitte" not in page                  # nur passende Treffer
    assert "Berlin-Mitte" in client.get("/?passend=0").get_data(as_text=True)
    assert client.get("/einstellungen").status_code == 200
    assert client.get("/protokoll").status_code == 200
    resp = client.post("/einstellungen", data={
        "mode": "miete", "max_price": "700", "min_rooms": "2", "min_area": "",
        "places": "Schloß Holte-Stukenbrock\nVerl", "domains": "immowelt.de",
        "interval_minutes": "60", "timelimit": "w", "max_results": "10",
        "backend": "ddgs", "pause_seconds": "2"})
    assert resp.status_code == 302
    page = client.get("/").get_data(as_text=True)
    assert "2 Zimmer Wohnung Liemke" in page           # 650 € <= 700 €
    assert "3-Zimmer-Wohnung mit Balkon" not in page   # 780 € > 700 €
    lid = app.config["agent"].storage.list_listings()[0]["id"]
    assert client.post(f"/listing/{lid}/status", data={"status": "favorit"}).status_code == 302
    assert client.get("/status").get_json()["counts"]["favoriten"] == 1


def test_extract_rent():
    from extractor import extract_rent
    r = extract_rent("Kaltmiete 780 €, Nebenkosten 150 €, Heizkosten 70 €")
    assert (r.cold, r.warm, r.warm_kind) == (780, 1000, "berechnet")
    r = extract_rent("Kaltmiete: 650,00 € zzgl. NK 120 €")
    assert (r.cold, r.warm, r.warm_kind) == (650, 770, "berechnet ohne Heizkosten")
    r = extract_rent("Warmmiete 940 € | 3 Zimmer | 75 m²")
    assert (r.cold, r.warm, r.warm_kind) == (None, 940, "angegeben")
    r = extract_rent("820 € warm, 680 € kalt")
    assert (r.cold, r.warm) == (680, 820)
    r = extract_rent("Miete 700 €, Warmwasser über Durchlauferhitzer")
    assert (r.cold, r.warm) == (None, None)


def test_price_basis_warm_and_cold():
    from extractor import _check_price, extract_rent
    warm = Criteria(max_price=900, price_basis="warm")
    cold = Criteria(max_price=900, price_basis="kalt")
    assert _check_price(warm, None, extract_rent("Warmmiete 950 €"))[0] is False
    assert _check_price(warm, None, extract_rent("Warmmiete 880 €"))[0] is True
    # nur Kaltmiete bekannt: unter Limit -> unbekannt, über Limit -> sicher zu teuer
    assert _check_price(warm, None, extract_rent("Kaltmiete 700 €"))[0] is None
    assert _check_price(warm, None, extract_rent("Kaltmiete 950 €"))[0] is False
    # berechnete Warmmiete ohne Heizkosten über Limit -> sicher zu teuer
    assert _check_price(warm, None, extract_rent("Kaltmiete 800 €, NK 150 €"))[0] is False
    assert _check_price(cold, None, extract_rent("Kaltmiete 800 €, NK 150 €"))[0] is True
    assert _check_price(cold, None, extract_rent("Warmmiete 850 €"))[0] is True
    assert _check_price(cold, 950, extract_rent("Miete 950 €"))[0] is False


def test_require_words():
    crit = Criteria(require_words=["Balkon"])
    t = FAKE_RESULTS[0]
    assert evaluate(t["title"], t["body"], t["url"], crit, PLACES, DISTRICTS).matches
    t = FAKE_RESULTS[1]
    assert not evaluate(t["title"], t["body"], t["url"], crit, PLACES, DISTRICTS).matches


def test_old_database_is_migrated(tmp_path):
    import sqlite3
    db = tmp_path / "alt.db"
    con = sqlite3.connect(db)
    con.execute("CREATE TABLE listings (id INTEGER PRIMARY KEY, url TEXT UNIQUE, link TEXT, "
                "title TEXT, body TEXT, domain TEXT, place TEXT, price REAL, rooms REAL, "
                "area REAL, is_listing INTEGER, matches INTEGER, score INTEGER, reasons TEXT, "
                "query TEXT, status TEXT DEFAULT 'neu', note TEXT DEFAULT '', first_seen TEXT, "
                "last_seen TEXT, notified INTEGER DEFAULT 0)")
    con.commit(); con.close()
    storage = Storage(str(db))
    Agent(storage, search_func=lambda q: FAKE_RESULTS).run_once()
    assert storage.list_listings(only_matches=False)
