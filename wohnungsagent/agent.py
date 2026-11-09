"""Der Suchagent: baut Suchanfragen, ruft die Websuche auf, bewertet und speichert.

Websuche:
- "ddgs"    – Python-Paket ddgs (Metasuche, u. a. DuckDuckGo), kein API-Schlüssel
              nötig: https://pypi.org/project/ddgs/
- "searxng" – eigene SearXNG-Instanz mit aktiviertem JSON-Format:
              https://docs.searxng.org/dev/search_api.html
"""
from __future__ import annotations

import logging
import os
import random
import smtplib
import threading
import time
from email.message import EmailMessage
from urllib.parse import urlsplit

import requests

from extractor import Criteria, canonical_url, evaluate
from region import DISTRICTS
from storage import Storage

log = logging.getLogger("wohnungsagent")


# ---------------------------------------------------------------- Websuche
class SearchError(Exception):
    pass


def search_ddgs(query: str, max_results: int, timelimit: str | None) -> list[dict]:
    try:
        from ddgs import DDGS
    except ImportError as exc:  # pragma: no cover
        raise SearchError("Paket 'ddgs' fehlt: pip install ddgs") from exc
    try:
        results = DDGS(timeout=15).text(query, region="de-de", safesearch="off",
                                        timelimit=timelimit or None,
                                        max_results=max_results)
    except Exception as exc:  # ddgs wirft u. a. DDGSException, RatelimitException
        raise SearchError(f"{type(exc).__name__}: {exc}") from exc
    return [{"title": r.get("title", ""), "url": r.get("href", ""), "body": r.get("body", "")}
            for r in results or []]


def search_searxng(base_url: str, query: str, max_results: int,
                   timelimit: str | None) -> list[dict]:
    if not base_url:
        raise SearchError("Keine SearXNG-URL in den Einstellungen hinterlegt")
    time_range = {"d": "day", "w": "week", "m": "month", "y": "year"}.get(timelimit or "")
    params = {"q": query, "format": "json", "language": "de-DE"}
    if time_range:
        params["time_range"] = time_range
    try:
        resp = requests.get(base_url.rstrip("/") + "/search", params=params, timeout=20)
        resp.raise_for_status()
        data = resp.json()
    except Exception as exc:
        raise SearchError(f"SearXNG: {exc}") from exc
    return [{"title": r.get("title", ""), "url": r.get("url", ""), "body": r.get("content", "")}
            for r in data.get("results", [])[:max_results]]


# ---------------------------------------------------------------- Anfragen
def build_queries(settings: dict) -> list[str]:
    what = "Wohnung mieten" if settings["mode"] == "miete" else "Wohnung kaufen"
    extra = settings.get("extra_terms", "").strip()
    queries = []
    for place in settings["places"]:
        base = f'{what} "{place}"'
        if extra:
            base += f" {extra}"
        for domain in settings["domains"]:
            queries.append(f"{base} site:{domain}")
        if settings.get("general_query", True):
            queries.append(base)
    return queries


def criteria_from(settings: dict) -> Criteria:
    def num(key):
        v = settings.get(key)
        return float(v) if v not in (None, "") else None
    return Criteria(
        mode=settings["mode"],
        price_basis=settings.get("price_basis", "warm"),
        max_price=num("max_price"),
        min_rooms=num("min_rooms"),
        min_area=num("min_area"),
        exclude_words=settings.get("exclude_words", []),
        include_words=settings.get("include_words", []),
        require_words=settings.get("require_words", []),
    )


# ---------------------------------------------------------------- Agent
class Agent:
    def __init__(self, storage: Storage, search_func=None):
        self.storage = storage
        self._search_func = search_func   # für Tests austauschbar
        self._run_lock = threading.Lock()
        self._wake = threading.Event()
        self._stop = threading.Event()
        self.status = {"running": False, "progress": "", "last_run": None, "next_run": None}
        self._thread: threading.Thread | None = None

    # -- einzelne Suche
    def _search(self, settings: dict, query: str) -> list[dict]:
        if self._search_func:
            return self._search_func(query)
        n = int(settings.get("max_results_per_query", 15))
        tl = settings.get("timelimit") or None
        if settings.get("backend") == "searxng":
            return search_searxng(settings.get("searxng_url", ""), query, n, tl)
        return search_ddgs(query, n, tl)

    def run_once(self) -> dict:
        """Ein kompletter Suchlauf. Gibt eine Zusammenfassung zurück."""
        if not self._run_lock.acquire(blocking=False):
            return {"skipped": True}
        try:
            self.status["running"] = True
            settings = self.storage.get_settings()
            criteria = criteria_from(settings)
            queries = build_queries(settings)
            run_id = self.storage.start_run()
            total = new = 0
            errors: list[str] = []
            pause = float(settings.get("pause_seconds", 3))
            for i, query in enumerate(queries, 1):
                if self._stop.is_set():
                    break
                self.status["progress"] = f"{i}/{len(queries)}: {query}"
                try:
                    results = self._search(settings, query)
                except SearchError as exc:
                    errors.append(f"{query}: {exc}")
                    log.warning("Suche fehlgeschlagen: %s – %s", query, exc)
                    results = []
                for r in results:
                    if not r.get("url"):
                        continue
                    total += 1
                    if self._store(r, query, criteria, settings):
                        new += 1
                if i < len(queries) and pause > 0 and not self._search_func:
                    time.sleep(pause + random.uniform(0, pause))  # höflich bleiben
            self.storage.finish_run(run_id, len(queries), total, new, errors)
            self._notify()
            self.status["last_run"] = time.strftime("%d.%m.%Y %H:%M")
            return {"queries": len(queries), "results": total, "new": new, "errors": errors}
        finally:
            self.status["running"] = False
            self.status["progress"] = ""
            self._run_lock.release()

    @staticmethod
    def _evaluation(title: str, body: str, url: str, criteria: Criteria,
                    settings: dict) -> dict:
        ev = evaluate(title, body or "", url, criteria, settings["places"], DISTRICTS)
        return {
            "place": ev.place,
            "price": ev.price,
            "cold": ev.cold,
            "warm": ev.warm,
            "warm_kind": ev.warm_kind,
            "rooms": ev.rooms,
            "area": ev.area,
            "is_listing": int(ev.is_listing),
            "matches": int(ev.matches),
            "score": ev.score,
            "reasons": "; ".join(ev.reasons),
        }

    def _store(self, r: dict, query: str, criteria: Criteria, settings: dict) -> bool:
        data = self._evaluation(r["title"], r.get("body", ""), r["url"], criteria, settings)
        data.update({
            "url": canonical_url(r["url"]),
            "link": r["url"],
            "title": r["title"] or r["url"],
            "body": r.get("body", ""),
            "domain": urlsplit(r["url"]).netloc.lower().removeprefix("www."),
            "query": query,
        })
        return self.storage.upsert_listing(data)

    def reevaluate(self) -> int:
        """Alle gespeicherten Treffer nach den aktuellen Kriterien neu bewerten."""
        settings = self.storage.get_settings()
        criteria = criteria_from(settings)
        rows = self.storage.all_listings()
        for row in rows:
            self.storage.update_evaluation(
                row["id"], self._evaluation(row["title"], row["body"], row["link"],
                                            criteria, settings))
        return len(rows)

    # -- Benachrichtigung per E-Mail (optional, über Umgebungsvariablen)
    def _notify(self) -> None:
        host, to = os.environ.get("SMTP_HOST"), os.environ.get("MAIL_TO")
        if not host or not to:
            return
        rows = self.storage.unnotified_matches()
        if not rows:
            return
        lines = [f"{r['title']}\n  {r['place'] or '?'} | kalt {r['cold'] or r['price'] or '?'} € | "
                 f"warm {r['warm'] or '?'} € | {r['rooms'] or '?'} Zi. | {r['area'] or '?'} m²\n"
                 f"  {r['link']}\n" for r in rows[:30]]
        msg = EmailMessage()
        msg["Subject"] = f"Wohnungsagent: {len(rows)} neue passende Treffer"
        msg["From"] = os.environ.get("MAIL_FROM", os.environ.get("SMTP_USER", to))
        msg["To"] = to
        msg.set_content("\n".join(lines) + "\nAngaben aus Suchmaschinen-Snippets – "
                        "bitte im Inserat prüfen.")
        try:
            port = int(os.environ.get("SMTP_PORT", "587"))
            with smtplib.SMTP(host, port, timeout=30) as s:
                s.starttls()
                if os.environ.get("SMTP_USER"):
                    s.login(os.environ["SMTP_USER"], os.environ.get("SMTP_PASS", ""))
                s.send_message(msg)
            self.storage.mark_notified([r["id"] for r in rows])
        except Exception as exc:
            log.warning("E-Mail-Versand fehlgeschlagen: %s", exc)

    # -- Hintergrundbetrieb
    def start_background(self) -> None:
        if self._thread and self._thread.is_alive():
            return
        self._thread = threading.Thread(target=self._loop, daemon=True, name="agent")
        self._thread.start()

    def trigger_now(self) -> None:
        threading.Thread(target=self.run_once, daemon=True, name="manual-run").start()

    def wake(self) -> None:
        """Nach Änderung der Einstellungen den Zeitplan neu berechnen."""
        self._wake.set()

    def _loop(self) -> None:
        while not self._stop.is_set():
            settings = self.storage.get_settings()
            interval = max(15, int(settings.get("interval_minutes", 180))) * 60
            if settings.get("agent_enabled"):
                self.status["next_run"] = time.strftime(
                    "%d.%m.%Y %H:%M", time.localtime(time.time() + interval))
                if self._wake.wait(interval):
                    self._wake.clear()
                    continue
                if self.storage.get_settings().get("agent_enabled"):
                    self.run_once()
            else:
                self.status["next_run"] = None
                self._wake.wait(60)
                self._wake.clear()
