"""Auswertung von Suchtreffern: Preis, Zimmer, Fläche, Ort, Inserats-Erkennung.

Die Werte stammen ausschließlich aus Titel und Kurztext (Snippet) der
Suchmaschine. Sie können daher fehlen oder veraltet sein und müssen immer
im Originalinserat geprüft werden.
"""
from __future__ import annotations

import re
import unicodedata
from dataclasses import dataclass, field
from urllib.parse import urlsplit, urlunsplit, parse_qsl, urlencode

_NUM = r"\d{1,3}(?:[.\s]\d{3})+|\d+"

PRICE_RE = re.compile(
    rf"(?:(?P<a>{_NUM})(?:,(?:\d{{1,2}}|-))?\s*(?:€|eur\b|euro\b))"
    rf"|(?:(?:€|eur)\s*(?P<b>{_NUM})(?:,\d{{1,2}})?)",
    re.IGNORECASE,
)
ROOMS_RE = re.compile(
    r"(?<![\d,.])(\d{1,2}(?:[,.]5)?)\s*(?:-|\s)?\s*(?:zimmer|zi\.|zkb|zkdb|raum|räume)",
    re.IGNORECASE,
)
AREA_RE = re.compile(
    r"(?<![\d,.])(\d{2,3}(?:[,.]\d{1,2})?)\s*(?:m²|m2|qm|quadratmeter)",
    re.IGNORECASE,
)

# URL-Muster einzelner Inserate (Exposés) bekannter Portale.
LISTING_URL_PATTERNS = [
    re.compile(r"immobilienscout24\.de/expose/\d+"),
    re.compile(r"immowelt\.de/(?:expose|classified-search/expose)/\w+"),
    re.compile(r"kleinanzeigen\.de/s-anzeige/"),
    re.compile(r"wg-gesucht\.de/.*\.\d+\.html"),
]

TRACKING_PARAMS = {"utm_source", "utm_medium", "utm_campaign", "utm_term",
                   "utm_content", "referrer", "ref", "gclid", "fbclid"}

SEARCH_GESUCH_RE = re.compile(r"\b(suche|gesucht|tausch|wohnungstausch)\b", re.IGNORECASE)


def normalize(text: str) -> str:
    """Kleinschreibung, ß->ss, Umlaute vereinfachen (für Ortsvergleiche)."""
    text = text.lower().replace("ß", "ss")
    text = unicodedata.normalize("NFKD", text)
    return "".join(c for c in text if not unicodedata.combining(c))


def canonical_url(url: str) -> str:
    """URL ohne Tracking-Parameter und Fragment – dient zur Duplikaterkennung."""
    parts = urlsplit(url.strip())
    query = [(k, v) for k, v in parse_qsl(parts.query) if k.lower() not in TRACKING_PARAMS]
    netloc = parts.netloc.lower().removeprefix("www.")
    path = parts.path.rstrip("/") or "/"
    return urlunsplit(("https", netloc, path, urlencode(query), ""))


def _to_number(raw: str) -> float:
    return float(re.sub(r"[.\s]", "", raw).replace(",", "."))


def extract_price(text: str, mode: str = "miete") -> float | None:
    """Erster plausibler Eurobetrag. Bei Miete 100–5000 €, bei Kauf 10.000–5 Mio €."""
    lo, hi = (100, 5000) if mode == "miete" else (10_000, 5_000_000)
    for m in PRICE_RE.finditer(text):
        raw = m.group("a") or m.group("b")
        try:
            value = _to_number(raw)
        except ValueError:
            continue
        if lo <= value <= hi:
            return value
    return None


# ---- Kalt-/Warmmiete und Nebenkosten ----------------------------------------
_AMT = r"(\d{1,3}(?:\.\d{3})+|\d+)(?:,(?:\d{1,2}|-{1,2}))?"
_PRE = r"\s*[:=]?\s*(?:ca\.?|rd\.?|von|nur)?\s*(?:€|eur)?\s*"
_EUR = r"\s*(?:€|eur\b|euro\b)"

WARM_LABELS = r"warmmiete|gesamtmiete|bruttowarmmiete|bruttomiete|miete\s+warm|warm"
COLD_LABELS = r"nettokaltmiete|kaltmiete|grundmiete|nettomiete|miete\s+kalt|kalt"
NK_LABELS = r"nebenkosten|betriebskosten|nk|bk"
HEAT_LABELS = r"heizkosten|hk"

WARM_RE = [re.compile(rf"(?<![a-zäöü])(?:{WARM_LABELS}){_PRE}{_AMT}", re.I),
           re.compile(rf"{_AMT}{_EUR}?\s*\(?\s*(?:warm|inkl\.?|inklusive|gesamt)\b", re.I)]
COLD_RE = [re.compile(rf"(?<![a-zäöü])(?:{COLD_LABELS}){_PRE}{_AMT}", re.I),
           re.compile(rf"{_AMT}{_EUR}?\s*\(?\s*(?:kalt|zzgl\.?|zuzüglich|netto)\b", re.I)]
NK_RE = [re.compile(rf"(?<![a-zäöü])(?:{NK_LABELS})(?:-?vorauszahlung)?{_PRE}{_AMT}", re.I)]
HEAT_RE = [re.compile(rf"(?<![a-zäöü])(?:{HEAT_LABELS}){_PRE}{_AMT}", re.I)]


def _first_amount(patterns, text: str, lo: float, hi: float) -> float | None:
    hits = []
    for pat in patterns:
        for m in pat.finditer(text):
            try:
                value = _to_number(m.group(1))
            except ValueError:
                continue
            if lo <= value <= hi:
                hits.append((m.start(), value))
    return min(hits)[1] if hits else None


@dataclass
class Rent:
    cold: float | None = None        # Kaltmiete (ausdrücklich genannt)
    warm: float | None = None        # Warmmiete (genannt oder berechnet)
    warm_kind: str | None = None     # "angegeben" | "berechnet" | "berechnet ohne Heizkosten"
    extra: float | None = None       # Nebenkosten (+ Heizkosten)


def extract_rent(text: str) -> Rent:
    """Liest Kaltmiete, Warmmiete und Nebenkosten aus einem Text.

    Warmmiete wird nur angegeben, wenn sie genannt ist oder sich aus
    Kaltmiete + Nebenkosten (+ Heizkosten) berechnen lässt – nie geschätzt.
    """
    r = Rent()
    r.cold = _first_amount(COLD_RE, text, 100, 5000)
    warm = _first_amount(WARM_RE, text, 100, 6000)
    nk = _first_amount(NK_RE, text, 10, 1500)
    heat = _first_amount(HEAT_RE, text, 10, 800)
    if nk is not None or heat is not None:
        r.extra = (nk or 0) + (heat or 0)
    if warm is not None and (r.cold is None or warm >= r.cold):
        r.warm, r.warm_kind = warm, "angegeben"
    elif r.cold is not None and nk is not None:
        r.warm = r.cold + nk + (heat or 0)
        r.warm_kind = "berechnet" if heat is not None else "berechnet ohne Heizkosten"
    return r


def extract_rooms(text: str) -> float | None:
    for m in ROOMS_RE.finditer(text):
        value = float(m.group(1).replace(",", "."))
        if 1 <= value <= 12:
            return value
    return None


def extract_area(text: str) -> float | None:
    for m in AREA_RE.finditer(text):
        value = float(m.group(1).replace(",", "."))
        if 10 <= value <= 500:
            return value
    return None


def detect_place(text: str, places: list[str], districts: dict[str, list[str]]) -> str | None:
    """Erster konfigurierter Ort (oder Ortsteil), der im Text vorkommt."""
    norm = normalize(text)
    candidates: list[tuple[str, str]] = []
    for place in places:
        candidates.append((place, place))
        for alias in districts.get(place, []):
            candidates.append((alias, place))
    # Längere Namen zuerst, damit "Stukenbrock-Senne" vor "Stukenbrock" greift.
    candidates.sort(key=lambda c: len(c[0]), reverse=True)
    for alias, place in candidates:
        if re.search(rf"(?<![a-z]){re.escape(normalize(alias))}(?![a-z])", norm):
            return place
    return None


def is_listing_url(url: str) -> bool:
    return any(p.search(url) for p in LISTING_URL_PATTERNS)


@dataclass
class Criteria:
    mode: str = "miete"              # "miete" oder "kauf"
    price_basis: str = "warm"        # bei Miete: "warm" oder "kalt"
    max_price: float | None = None
    min_rooms: float | None = None
    min_area: float | None = None
    exclude_words: list[str] = field(default_factory=list)
    include_words: list[str] = field(default_factory=list)
    require_words: list[str] = field(default_factory=list)


@dataclass
class Evaluation:
    price: float | None
    cold: float | None
    warm: float | None
    warm_kind: str | None
    rooms: float | None
    area: float | None
    place: str | None
    is_listing: bool
    matches: bool
    score: int
    reasons: list[str]


def evaluate(title: str, body: str, url: str, criteria: Criteria,
             places: list[str], districts: dict[str, list[str]]) -> Evaluation:
    text = f"{title} \n {body}"
    norm = normalize(text)
    price = extract_price(text, criteria.mode)
    rent = extract_rent(text) if criteria.mode == "miete" else Rent()
    rooms = extract_rooms(text)
    area = extract_area(text)
    place = detect_place(f"{text} {url}", places, districts)
    listing = is_listing_url(url)

    reasons: list[str] = []
    score = 50
    matches = True

    if place is None:
        matches = False
        reasons.append("kein Ort der Suchregion erkannt")
    else:
        score += 10

    if criteria.max_price is not None:
        ok, reason = _check_price(criteria, price, rent)
        if ok is None:
            score -= 5
        elif ok:
            score += 15
        else:
            matches = False
        if reason:
            reasons.append(reason)
    if criteria.min_rooms is not None:
        if rooms is None:
            score -= 5
            reasons.append("Zimmerzahl unbekannt")
        elif rooms < criteria.min_rooms:
            matches = False
            reasons.append(f"{rooms:g} Zimmer < {criteria.min_rooms:g}")
        else:
            score += 10
    if criteria.min_area is not None:
        if area is None:
            score -= 5
            reasons.append("Fläche unbekannt")
        elif area < criteria.min_area:
            matches = False
            reasons.append(f"{area:g} m² < {criteria.min_area:g} m²")
        else:
            score += 10

    for word in criteria.exclude_words:
        if word and normalize(word) in norm:
            matches = False
            reasons.append(f"Ausschlusswort „{word}“")
    for word in criteria.require_words:
        if word and normalize(word) not in norm:
            matches = False
            reasons.append(f"Pflichtmerkmal „{word}“ nicht erwähnt")
    for word in criteria.include_words:
        if word and normalize(word) in norm:
            score += 5

    if SEARCH_GESUCH_RE.search(title):
        score -= 15
        reasons.append("evtl. Gesuch statt Angebot")
    if listing:
        score += 10
    else:
        score -= 10
        reasons.append("Übersichts-/Suchseite, kein Einzelinserat")

    return Evaluation(price, rent.cold, rent.warm, rent.warm_kind, rooms, area, place,
                      listing, matches, max(0, min(100, score)), reasons)


def _eur(v: float) -> str:
    return f"{v:,.0f} €".replace(",", ".")


def _check_price(c: Criteria, price: float | None, rent: Rent) -> tuple[bool | None, str]:
    """(True=passt, False=zu teuer, None=unbekannt) und Begründung.

    Ausgeschlossen wird nur, was sicher zu teuer ist: Warmmiete >= Kaltmiete,
    ein Preis ohne Angabe ist also in jedem Fall eine untere Grenze der Warmmiete.
    """
    limit = c.max_price
    if c.mode != "miete":
        if price is None:
            return None, "Preis unbekannt"
        return (price <= limit), ("" if price <= limit else f"Preis {_eur(price)} > {_eur(limit)}")

    if c.price_basis == "warm":
        if rent.warm is not None:
            note = "" if rent.warm_kind == "angegeben" else f" ({rent.warm_kind})"
            if rent.warm > limit:
                return False, f"Warmmiete {_eur(rent.warm)}{note} > {_eur(limit)}"
            if rent.warm_kind == "berechnet ohne Heizkosten":
                return True, "Warmmiete ohne Heizkosten berechnet"
            return True, ""
        lower = rent.cold if rent.cold is not None else price
        if lower is not None and lower > limit:
            return False, f"schon {'Kaltmiete' if rent.cold else 'Preis'} {_eur(lower)} > {_eur(limit)} warm"
        if rent.cold is not None:
            return None, f"Warmmiete unbekannt (Kaltmiete {_eur(rent.cold)})"
        if price is not None:
            return None, f"Warmmiete unbekannt ({_eur(price)} ohne Angabe kalt/warm)"
        return None, "Preis unbekannt"

    # Kaltmiete als Maßstab
    cold = rent.cold if rent.cold is not None else price
    if cold is not None:
        if cold > limit:
            return False, f"Kaltmiete {_eur(cold)} > {_eur(limit)}"
        return True, "" if rent.cold is not None else "Preis ohne Angabe kalt/warm"
    if rent.warm is not None and rent.warm <= limit:
        return True, "nur Warmmiete bekannt"
    return None, "Kaltmiete unbekannt"
