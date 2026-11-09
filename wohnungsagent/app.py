"""Webinterface des Wohnungssuchagenten (Flask).

Start:  python app.py            -> http://127.0.0.1:5000
"""
from __future__ import annotations

import argparse
import logging
import os

from flask import Flask, flash, jsonify, redirect, render_template, request, url_for

from agent import Agent, build_queries
from region import DEFAULT_PLACES
from storage import Storage

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
STATUSES = ["neu", "gesehen", "favorit", "ausgeblendet"]


def _split_lines(text: str) -> list[str]:
    items = []
    for line in text.replace(",", "\n").splitlines():
        line = line.strip()
        if line and line not in items:
            items.append(line)
    return items


def _num(text: str):
    text = (text or "").strip().replace(",", ".")
    if not text:
        return None
    try:
        return float(text)
    except ValueError:
        return None


def create_app(db_path: str | None = None, search_func=None, start_agent=True) -> Flask:
    app = Flask(__name__)
    app.secret_key = os.environ.get("SECRET_KEY", os.urandom(16))
    storage = Storage(db_path or os.path.join(BASE_DIR, "wohnungen.db"))
    agent = Agent(storage, search_func=search_func)
    app.config["agent"] = agent
    if start_agent:
        agent.start_background()

    @app.template_filter("eur")
    def eur(value):
        if value is None:
            return "–"
        return f"{value:,.0f} €".replace(",", ".")

    @app.template_filter("num")
    def num(value):
        return "–" if value is None else f"{value:g}".replace(".", ",")

    @app.route("/")
    def index():
        args = request.args
        passend = args.getlist("passend")
        only_matches = (not passend) or "1" in passend
        only_listings = args.get("inserate", "0") == "1"
        status = args.get("status", "aktiv")
        place = args.get("ort") or None
        sort = args.get("sort", "score")
        rows = storage.list_listings(only_matches=only_matches, status=status or None,
                                     place=place, only_listings=only_listings, sort=sort)
        return render_template(
            "index.html", rows=rows, counts=storage.counts(), places=storage.places_in_db(),
            statuses=STATUSES, f={"passend": only_matches, "inserate": only_listings,
                                  "status": status, "ort": place or "", "sort": sort},
            agent=agent.status, settings=storage.get_settings())

    @app.post("/listing/<int:lid>/status")
    def set_status(lid):
        status = request.form.get("status")
        if status in STATUSES:
            storage.set_status(lid, status)
        return redirect(request.referrer or url_for("index"))

    @app.post("/listing/<int:lid>/note")
    def set_note(lid):
        storage.set_note(lid, request.form.get("note", "")[:2000])
        return redirect(request.referrer or url_for("index"))

    @app.post("/suche")
    def run_now():
        if agent.status["running"]:
            flash("Es läuft bereits eine Suche.")
        else:
            agent.trigger_now()
            flash("Suche gestartet – die Seite aktualisiert sich automatisch.")
        return redirect(url_for("index"))

    @app.get("/status")
    def status():
        return jsonify(agent.status | {"counts": storage.counts()})

    @app.route("/einstellungen", methods=["GET", "POST"])
    def settings_view():
        if request.method == "POST":
            form = request.form
            places = _split_lines(form.get("places", ""))
            domains = [d.lower().removeprefix("https://").removeprefix("www.").strip("/")
                       for d in _split_lines(form.get("domains", ""))]
            if not places:
                flash("Bitte mindestens einen Ort angeben.")
                return redirect(url_for("settings_view"))
            storage.save_settings({
                "mode": "kauf" if form.get("mode") == "kauf" else "miete",
                "max_price": _num(form.get("max_price")),
                "min_rooms": _num(form.get("min_rooms")),
                "min_area": _num(form.get("min_area")),
                "places": places,
                "domains": domains,
                "general_query": form.get("general_query") == "on",
                "exclude_words": _split_lines(form.get("exclude_words", "")),
                "include_words": _split_lines(form.get("include_words", "")),
                "extra_terms": form.get("extra_terms", "").strip(),
                "interval_minutes": max(15, int(_num(form.get("interval_minutes")) or 180)),
                "agent_enabled": form.get("agent_enabled") == "on",
                "timelimit": form.get("timelimit", "w") if form.get("timelimit") in
                ("d", "w", "m", "y", "") else "w",
                "max_results_per_query": max(1, min(50, int(_num(form.get("max_results")) or 15))),
                "backend": "searxng" if form.get("backend") == "searxng" else "ddgs",
                "searxng_url": form.get("searxng_url", "").strip(),
                "pause_seconds": max(0.0, _num(form.get("pause_seconds")) or 0),
            })
            agent.wake()
            n = agent.reevaluate()
            flash(f"Einstellungen gespeichert, {n} gespeicherte Treffer neu bewertet.")
            return redirect(url_for("settings_view"))
        settings = storage.get_settings()
        return render_template("settings.html", s=settings, queries=build_queries(settings),
                               default_places=DEFAULT_PLACES, agent=agent.status)

    @app.get("/protokoll")
    def runs():
        return render_template("runs.html", runs=storage.list_runs(), agent=agent.status)

    @app.post("/aufraeumen")
    def cleanup():
        n = storage.delete_hidden()
        flash(f"{n} ausgeblendete Treffer gelöscht.")
        return redirect(url_for("index"))

    return app


def main():
    parser = argparse.ArgumentParser(description="Wohnungssuchagent – Webinterface")
    parser.add_argument("--host", default="127.0.0.1",
                        help="Adresse (Standard 127.0.0.1 = nur dieser Rechner)")
    parser.add_argument("--port", type=int, default=5000)
    parser.add_argument("--db", default=None, help="Pfad zur SQLite-Datei")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    app = create_app(args.db)
    app.run(host=args.host, port=args.port, debug=False, threaded=True)


if __name__ == "__main__":
    main()
