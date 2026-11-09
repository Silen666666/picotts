"""SQLite-Speicher für Treffer, Suchläufe und Einstellungen."""
from __future__ import annotations

import json
import sqlite3
import threading
from contextlib import contextmanager
from datetime import datetime

from region import DEFAULT_DOMAINS, DEFAULT_PLACES

SCHEMA = """
CREATE TABLE IF NOT EXISTS listings (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    url TEXT UNIQUE NOT NULL,
    link TEXT NOT NULL,
    title TEXT NOT NULL,
    body TEXT,
    domain TEXT,
    place TEXT,
    price REAL,
    rooms REAL,
    area REAL,
    is_listing INTEGER,
    matches INTEGER,
    score INTEGER,
    reasons TEXT,
    query TEXT,
    status TEXT DEFAULT 'neu',          -- neu | gesehen | favorit | ausgeblendet
    note TEXT DEFAULT '',
    first_seen TEXT,
    last_seen TEXT,
    notified INTEGER DEFAULT 0
);
CREATE TABLE IF NOT EXISTS runs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    started TEXT,
    finished TEXT,
    queries INTEGER DEFAULT 0,
    results INTEGER DEFAULT 0,
    new_results INTEGER DEFAULT 0,
    errors TEXT DEFAULT ''
);
CREATE TABLE IF NOT EXISTS settings (
    key TEXT PRIMARY KEY,
    value TEXT
);
"""

DEFAULT_SETTINGS = {
    "mode": "miete",
    "max_price": 900,
    "min_rooms": 2,
    "min_area": 50,
    "places": [p for p, _, active in DEFAULT_PLACES if active],
    "domains": DEFAULT_DOMAINS,
    "general_query": True,          # zusätzlich ohne site:-Filter suchen
    "exclude_words": ["Tausch", "Gesuch", "WBS erforderlich"],
    "include_words": ["Balkon", "Garten", "Stellplatz"],
    "extra_terms": "",
    "interval_minutes": 180,
    "agent_enabled": False,
    "timelimit": "w",               # d | w | m | y | "" (keine Begrenzung)
    "max_results_per_query": 15,
    "backend": "ddgs",              # ddgs | searxng
    "searxng_url": "",
    "pause_seconds": 3,
}


def now() -> str:
    return datetime.now().isoformat(timespec="seconds")


class Storage:
    def __init__(self, path: str):
        self.path = path
        self._lock = threading.Lock()
        with self.conn() as c:
            c.executescript(SCHEMA)

    @contextmanager
    def conn(self):
        c = sqlite3.connect(self.path, timeout=30)
        c.row_factory = sqlite3.Row
        try:
            yield c
            c.commit()
        finally:
            c.close()

    # ---------- Einstellungen ----------
    def get_settings(self) -> dict:
        settings = dict(DEFAULT_SETTINGS)
        with self.conn() as c:
            for row in c.execute("SELECT key, value FROM settings"):
                settings[row["key"]] = json.loads(row["value"])
        return settings

    def save_settings(self, values: dict) -> None:
        with self.conn() as c:
            for key, value in values.items():
                c.execute("INSERT OR REPLACE INTO settings(key, value) VALUES (?, ?)",
                          (key, json.dumps(value, ensure_ascii=False)))

    # ---------- Treffer ----------
    def upsert_listing(self, data: dict) -> bool:
        """Speichert einen Treffer. Gibt True zurück, wenn er neu ist."""
        with self._lock, self.conn() as c:
            row = c.execute("SELECT id FROM listings WHERE url = ?", (data["url"],)).fetchone()
            ts = now()
            if row:
                c.execute(
                    """UPDATE listings SET title=?, body=?, place=?, price=?, rooms=?, area=?,
                       is_listing=?, matches=?, score=?, reasons=?, last_seen=? WHERE id=?""",
                    (data["title"], data["body"], data["place"], data["price"], data["rooms"],
                     data["area"], data["is_listing"], data["matches"], data["score"],
                     data["reasons"], ts, row["id"]),
                )
                return False
            c.execute(
                """INSERT INTO listings (url, link, title, body, domain, place, price, rooms, area,
                   is_listing, matches, score, reasons, query, first_seen, last_seen)
                   VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
                (data["url"], data["link"], data["title"], data["body"], data["domain"],
                 data["place"], data["price"], data["rooms"], data["area"], data["is_listing"],
                 data["matches"], data["score"], data["reasons"], data["query"], ts, ts),
            )
            return True

    def list_listings(self, *, only_matches=False, status=None, place=None,
                      only_listings=False, sort="score") -> list[sqlite3.Row]:
        where, args = [], []
        if only_matches:
            where.append("matches = 1")
        if only_listings:
            where.append("is_listing = 1")
        if status == "aktiv":
            where.append("status != 'ausgeblendet'")
        elif status:
            where.append("status = ?")
            args.append(status)
        if place:
            where.append("place = ?")
            args.append(place)
        order = {
            "score": "score DESC, first_seen DESC",
            "neu": "first_seen DESC",
            "preis": "price IS NULL, price ASC",
        }.get(sort, "score DESC")
        sql = "SELECT * FROM listings"
        if where:
            sql += " WHERE " + " AND ".join(where)
        sql += f" ORDER BY {order} LIMIT 500"
        with self.conn() as c:
            return c.execute(sql, args).fetchall()

    def all_listings(self) -> list[sqlite3.Row]:
        with self.conn() as c:
            return c.execute("SELECT * FROM listings").fetchall()

    def update_evaluation(self, listing_id: int, ev: dict) -> None:
        with self.conn() as c:
            c.execute("UPDATE listings SET place=?, price=?, rooms=?, area=?, is_listing=?, "
                      "matches=?, score=?, reasons=? WHERE id=?",
                      (ev["place"], ev["price"], ev["rooms"], ev["area"], ev["is_listing"],
                       ev["matches"], ev["score"], ev["reasons"], listing_id))

    def set_status(self, listing_id: int, status: str) -> None:
        with self.conn() as c:
            c.execute("UPDATE listings SET status=? WHERE id=?", (status, listing_id))

    def set_note(self, listing_id: int, note: str) -> None:
        with self.conn() as c:
            c.execute("UPDATE listings SET note=? WHERE id=?", (note, listing_id))

    def unnotified_matches(self) -> list[sqlite3.Row]:
        with self.conn() as c:
            return c.execute(
                "SELECT * FROM listings WHERE matches=1 AND notified=0 "
                "AND status != 'ausgeblendet' ORDER BY score DESC").fetchall()

    def mark_notified(self, ids: list[int]) -> None:
        with self.conn() as c:
            c.executemany("UPDATE listings SET notified=1 WHERE id=?", [(i,) for i in ids])

    def counts(self) -> dict:
        with self.conn() as c:
            row = c.execute(
                """SELECT COUNT(*) total,
                          SUM(matches = 1 AND status != 'ausgeblendet') passend,
                          SUM(status = 'neu' AND matches = 1) neu,
                          SUM(status = 'favorit') favoriten
                   FROM listings""").fetchone()
            return {k: (row[k] or 0) for k in row.keys()}

    def places_in_db(self) -> list[str]:
        with self.conn() as c:
            return [r[0] for r in c.execute(
                "SELECT DISTINCT place FROM listings WHERE place IS NOT NULL ORDER BY place")]

    def delete_hidden(self) -> int:
        with self.conn() as c:
            return c.execute("DELETE FROM listings WHERE status='ausgeblendet'").rowcount

    # ---------- Suchläufe ----------
    def start_run(self) -> int:
        with self.conn() as c:
            return c.execute("INSERT INTO runs (started) VALUES (?)", (now(),)).lastrowid

    def finish_run(self, run_id: int, queries: int, results: int, new: int, errors: list[str]):
        with self.conn() as c:
            c.execute("UPDATE runs SET finished=?, queries=?, results=?, new_results=?, errors=? "
                      "WHERE id=?", (now(), queries, results, new, "\n".join(errors), run_id))

    def list_runs(self, limit=50) -> list[sqlite3.Row]:
        with self.conn() as c:
            return c.execute("SELECT * FROM runs ORDER BY id DESC LIMIT ?", (limit,)).fetchall()
