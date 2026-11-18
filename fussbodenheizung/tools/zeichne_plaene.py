# -*- coding: utf-8 -*-
"""
Erzeugt die Pläne als SVG (nur Python-Standardbibliothek):
  docs/stromlaufplan.svg     Stromlaufplan Unterverteilung (aufgelöste Darstellung)
  docs/systemuebersicht.svg  Blockschaltbild des Gesamtsystems
  docs/hutschiene.svg        Belegungsplan der Hutschienen (Teilungseinheiten)

Schaltzeichen sinngemäß nach DIN EN 60617 (IEC 60617): Schließer, Relaisspule,
Umrichter (Rechteck mit Diagonale), Klemme (Kreis), Leitungsschutzschalter,
allgemeines Betriebsmittel (Rechteck). Betriebsmittelkennzeichen nach
DIN EN 81346-2 (Q = Schalten Energie, K = Relais, T = Netzteil, A = Baugruppe,
X = Klemme, M = Antrieb).

Aufruf:  python3 tools/zeichne_plaene.py [Anzahl_Heizkanäle]   (Standard 8)
"""
import os
import sys

HIER = os.path.dirname(os.path.abspath(__file__))
DOCS = os.path.join(HIER, "..", "docs")

GPIO = [16, 17, 18, 19, 21, 22, 23, 25]
GPIO_ANF = 26


class SVG:
    def __init__(self, b, h):
        self.b, self.h = b, h
        self.e = []

    def linie(self, x1, y1, x2, y2, w=1.6, farbe="#000", strich=None):
        d = ' stroke-dasharray="%s"' % strich if strich else ""
        self.e.append('<line x1="%g" y1="%g" x2="%g" y2="%g" stroke="%s" stroke-width="%g"%s/>'
                      % (x1, y1, x2, y2, farbe, w, d))

    def pfad(self, pts, w=1.6, farbe="#000"):
        p = " ".join("%g,%g" % xy for xy in pts)
        self.e.append('<polyline points="%s" fill="none" stroke="%s" stroke-width="%g"/>'
                      % (p, farbe, w))

    def rechteck(self, x, y, b, h, w=1.6, fuell="none", strich=None, farbe="#000"):
        d = ' stroke-dasharray="%s"' % strich if strich else ""
        self.e.append('<rect x="%g" y="%g" width="%g" height="%g" fill="%s" stroke="%s" '
                      'stroke-width="%g"%s/>' % (x, y, b, h, fuell, farbe, w, d))

    def kreis(self, x, y, r, fuell="#fff", w=1.4):
        self.e.append('<circle cx="%g" cy="%g" r="%g" fill="%s" stroke="#000" stroke-width="%g"/>'
                      % (x, y, r, fuell, w))

    def punkt(self, x, y):
        self.kreis(x, y, 3.2, fuell="#000", w=0)

    def text(self, x, y, t, g=12, anker="start", fett=False, farbe="#000", dreh=0):
        f = ' font-weight="bold"' if fett else ""
        r = ' transform="rotate(%g %g %g)"' % (dreh, x, y) if dreh else ""
        t = t.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
        self.e.append('<text x="%g" y="%g" font-size="%g" text-anchor="%s" fill="%s"%s%s>%s</text>'
                      % (x, y, g, anker, farbe, f, r, t))

    def speichern(self, name):
        kopf = ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
                'viewBox="0 0 %d %d" font-family="DejaVu Sans, Arial, sans-serif">\n'
                '<rect width="100%%" height="100%%" fill="#fff"/>\n' % (self.b, self.h, self.b, self.h))
        os.makedirs(DOCS, exist_ok=True)
        pfad = os.path.join(DOCS, name)
        with open(pfad, "w", encoding="utf-8") as f:
            f.write(kopf + "\n".join(self.e) + "\n</svg>\n")
        print("geschrieben:", os.path.normpath(pfad))


# --------------------------------------------------------------- Schaltzeichen
def schliesser(s, x, y1, y2, name, k1="11", k2="14"):
    """Schließer (Arbeitskontakt) senkrecht zwischen y1 (oben) und y2 (unten)."""
    m = (y1 + y2) / 2.0
    s.linie(x, y1, x, m - 12)
    s.linie(x, m + 12, x, y2)
    s.linie(x, m + 12, x - 12, m - 14)           # beweglicher Kontakt
    s.text(x + 6, m - 14, k1, 9)
    s.text(x + 6, m + 22, k2, 9)
    s.text(x - 16, m + 4, name, 11, "end", fett=True)


def spule(s, x, y, name):
    """Relaisspule (Rechteck) zentriert bei x, y; Anschlüsse A1 oben, A2 unten."""
    s.rechteck(x - 12, y - 20, 24, 40, fuell="#fff")
    s.text(x + 16, y - 22, "A1", 9)
    s.text(x + 16, y + 30, "A2", 9)
    s.text(x - 16, y + 4, name, 11, "end", fett=True)


def klemme(s, x, y, name, rechts=True):
    s.kreis(x, y, 4.5)
    s.text(x + (9 if rechts else -9), y + 4, name, 10, "start" if rechts else "end")


def umrichter(s, x, y, b, h, oben="~", unten="="):
    s.rechteck(x, y, b, h, fuell="#fff")
    s.linie(x, y + h, x + b, y)
    s.text(x + 8, y + 18, oben, 16)
    s.text(x + b - 8, y + h - 6, unten, 16, "end")


def ls_schalter(s, x, y1, y2, name, daten):
    """Leitungsschutzschalter: Schließer mit Kreuz (Leistungsschalterfunktion)
    und Kästchen für thermische/magnetische Auslösung (vereinfacht)."""
    m = (y1 + y2) / 2.0
    s.linie(x, y1, x, m - 12)
    s.linie(x, m + 12, x, y2)
    s.linie(x, m + 12, x - 12, m - 14)
    s.linie(x - 4, m - 16, x + 4, m - 8)          # Kreuz am Festkontakt
    s.linie(x - 4, m - 8, x + 4, m - 16)
    s.text(x + 10, m - 2, name, 11, fett=True)
    s.text(x + 10, m + 12, daten, 10)


# --------------------------------------------------------------- Stromlaufplan
def stromlaufplan(n=8):
    dx = 110
    x0 = 720
    xs = [x0 + i * dx for i in range(n)]
    x_anf = x0 + n * dx                       # Spalte K9 (Wärmeanforderung)
    breite = x_anf + 170
    s = SVG(breite, 1230)

    y_L, y_N, y_PE = 120, 420, 445
    y_p24, y_0v = 590, 1030

    s.text(20, 28, "Stromlaufplan Fußbodenheizungsregelung – Unterverteilung", 18, fett=True)
    s.text(20, 48, "Hauptstromkreis 230 V AC (oben)  ·  Steuerstromkreis 24 V DC SELV (unten)  ·  "
           "Ausführung nur durch Elektrofachkraft", 12)

    # ---- Einspeisung
    x_L, x_N, x_PE = 110, 70, 40
    s.text(x_PE, 70, "PE", 11, "middle", fett=True)
    s.text(x_N, 70, "N", 11, "middle", fett=True)
    s.text(x_L, 70, "L", 11, "middle", fett=True)
    s.text(130, 70, "aus UV, hinter RCD 30 mA (bestehend)", 10)
    s.linie(x_L, 76, x_L, 78)
    ls_schalter(s, x_L, 78, y_L, "-Q1", "B6 A")
    s.punkt(x_L, y_L)
    s.linie(x_L, y_L, x_anf - 40, y_L)
    s.text(x_anf - 36, y_L + 4, "L'", 11, fett=True)
    s.linie(x_N, 76, x_N, y_N)
    s.punkt(x_N, y_N)
    s.linie(x_N, y_N, xs[-1] + 30, y_N)
    s.text(xs[-1] + 34, y_N + 4, "N", 11, fett=True)
    s.linie(x_PE, 76, x_PE, y_PE, strich=None)
    s.linie(x_PE, y_PE, xs[-1] + 30, y_PE, w=1.2)
    s.text(xs[-1] + 34, y_PE + 4, "PE", 11, fett=True)

    # ---- Lastkreise K1..Kn -> Stellantriebe
    for i, x in enumerate(xs):
        s.punkt(x, y_L)
        schliesser(s, x, y_L, 200, "-K%d" % (i + 1))
        s.linie(x, 200, x, 226)
        klemme(s, x, 230, "X2:%d" % (i + 1))
        s.linie(x, 234.5, x, 270, strich="6 4")
        s.rechteck(x - 22, 270, 44, 52, fuell="#fff")
        s.text(x, 300, "M", 16, "middle")
        s.text(x + 26, 286, "-M%d" % (i + 1), 10, fett=True)
        s.text(x + 26, 299, "230 V", 9)
        s.text(x + 26, 311, "NC", 9)
        s.linie(x, 322, x, 360, strich="6 4")
        klemme(s, x, 364, "X2:N%d" % (i + 1))
        s.linie(x, 368.5, x, y_N)
        s.punkt(x, y_N)
    s.linie(xs[0] - 50, 252, xs[-1] + 60, 252, w=0.8, farbe="#666", strich="2 3")
    s.linie(xs[0] - 50, 340, xs[-1] + 60, 340, w=0.8, farbe="#666", strich="2 3")
    s.text(xs[0] - 54, 262, "Feldleitung zum", 9, "end", farbe="#444")
    s.text(xs[0] - 54, 274, "Heizkreisverteiler", 9, "end", farbe="#444")
    s.text(xs[0] - 54, 300, "Stellantriebe", 9, "end", farbe="#444")
    s.text(xs[0] - 54, 312, "(2-adrig, Schutzkl. II)", 9, "end", farbe="#444")

    # PE-Klemme für Schutzleiter der Feldleitung
    klemme(s, xs[0] - 90, y_PE, "X2:PE", rechts=False)

    # ---- K9 potentialfreier Kontakt Wärmeanforderung
    klemme(s, x_anf, 150, "X3:1")
    s.linie(x_anf, 154.5, x_anf, 165)
    schliesser(s, x_anf, 165, 235, "-K%d" % (n + 1))
    klemme(s, x_anf, 239.5, "X3:2")
    s.text(x_anf - 10, 270, "potentialfrei:", 10)
    s.text(x_anf - 10, 284, "Wärmeanforderung", 10)
    s.text(x_anf - 10, 298, "an Wärmeerzeuger /", 10)
    s.text(x_anf - 10, 312, "Pumpenfreigabe", 10)
    s.text(x_anf - 10, 326, "(optional)", 10)

    # ---- Netzteil T1
    tx, ty = 150, 470
    s.linie(180, y_L, 180, ty)                  # L' -> T1 (kreuzt N ohne Verbindung)
    s.punkt(180, y_L)
    s.linie(240, y_N, 240, ty)
    s.punkt(240, y_N)
    umrichter(s, tx, ty, 140, 70)
    s.text(176, ty - 4, "L", 9)
    s.text(244, ty - 4, "N", 9)
    s.text(tx + 150, ty + 24, "-T1", 11, fett=True)
    s.text(tx + 150, ty + 38, "Hutschienen-Netzteil", 9)
    s.text(tx + 150, ty + 50, "230 V AC / 24 V DC, 15 W", 9)
    s.linie(260, ty + 70, 260, y_p24)
    s.text(264, ty + 84, "+V", 9)
    s.linie(170, ty + 70, 170, y_0v)
    s.text(174, ty + 84, "−V", 9)
    s.punkt(260, y_p24)
    s.punkt(170, y_0v)
    s.linie(260, y_p24, breite - 20, y_p24)
    s.text(breite - 20, y_p24 - 6, "+24 V DC", 11, "end", fett=True)
    s.linie(170, y_0v, breite - 20, y_0v)
    s.text(breite - 20, y_0v - 6, "0 V", 11, "end", fett=True)

    # ---- Relaisspulen
    y_sp = 660
    for i, x in enumerate(xs + [x_anf]):
        s.punkt(x, y_p24)
        s.linie(x, y_p24, x, y_sp - 20)
        spule(s, x, y_sp, "-K%d" % (i + 1))
        s.linie(x, y_sp + 20, x, 760)

    # ---- Treiber A2 (ULN2803A) für K1..K8, A3 für K9
    def treiber(xa, xb, spalten, name, pins_aus, pins_ein, x_com, x_gnd):
        s.rechteck(xa, 760, xb - xa, 64, fuell="#fff")
        s.text(xa + 6, 796, name, 11, fett=True)
        s.text(xa + 6, 810, "ULN2803A", 9)
        for x, pa, pe in zip(spalten, pins_aus, pins_ein):
            s.text(x + 4, 772, str(pa), 8)
            s.text(x + 4, 820, str(pe), 8)
        # COM (Pin 10) an +24 V (Freilaufdioden), GND (Pin 9) an 0 V
        s.linie(x_com, 760, x_com, y_p24)
        s.punkt(x_com, y_p24)
        s.text(x_com + 3, 754, "10", 8)
        s.linie(x_gnd, 824, x_gnd, y_0v)
        s.punkt(x_gnd, y_0v)
        s.text(x_gnd + 3, 836, "9", 8)

    treiber(xs[0] - 60, xs[-1] + 40, xs, "-A2", range(18, 10, -1), range(1, 9),
            xs[0] - 45, xs[0] - 52)
    treiber(x_anf - 40, x_anf + 70, [x_anf], "-A3", [18], [1], x_anf + 50, x_anf + 60)
    s.text(x_anf - 36, 852, "Kanäle 2..8 frei", 8)
    s.text(x_anf - 36, 862, "(Erweiterung)", 8)

    # ---- ESP32 A1 mit Pulldowns
    y_esp = 900
    for i, x in enumerate(xs + [x_anf]):
        s.linie(x, 824, x, y_esp)
        g = GPIO[i] if i < len(GPIO) else GPIO_ANF
        s.text(x + 4, y_esp - 6, "GPIO%d" % g, 8)
    s.rechteck(xs[0] - 30, y_esp, x_anf + 30 - (xs[0] - 30), 60, fuell="#fff")
    s.text(xs[0] - 22, y_esp + 26, "-A1  ESP32-DevKitC-32U (ESP32-WROOM-32U, externe 2,4-GHz-Antenne)", 11,
           fett=True)
    s.text(xs[0] - 22, y_esp + 44, "MicroPython · Pulldown 10 kΩ je Ausgang nach 0 V auf der Trägerplatine "
           "(Relais bleiben beim Booten AUS)", 9)
    s.linie((xs[0] + x_anf) / 2.0, y_esp + 60, (xs[0] + x_anf) / 2.0, y_0v)
    s.punkt((xs[0] + x_anf) / 2.0, y_0v)
    s.text((xs[0] + x_anf) / 2.0 + 4, y_esp + 74, "GND", 8)

    # ---- DC/DC U1 24 V -> 5 V
    ux, uy = 400, y_esp
    s.linie(430, y_p24, 430, uy)
    s.punkt(430, y_p24)
    umrichter(s, ux, uy, 110, 60, oben="=", unten="=")
    s.text(ux, uy + 78, "-U1  DC/DC 24 V → 5 V, 1 A", 10, fett=True)
    s.text(ux, uy + 92, "z.B. TRACO TSR 1-2450", 9)
    s.linie(490, uy + 60, 490, y_0v)
    s.punkt(490, y_0v)
    s.linie(ux + 110, uy + 20, xs[0] - 30, uy + 20)
    s.text(ux + 116, uy + 15, "+5 V → 5V-Pin", 9)

    # ---- Legende
    lx, ly = 20, 1075
    s.text(lx, ly, "Betriebsmittel", 11, fett=True)
    zeilen = ["-Q1  LS-Schalter B6, 1-polig (Steuerung + Antriebe)",
              "-T1  Netzteil 24 V DC (z.B. MEAN WELL HDR-15-24)",
              "-U1  DC/DC-Wandler 5 V für ESP32",
              "-A1  ESP32 Zentrale",
              "-A2/-A3  Relaistreiber ULN2803A",
              "-K1…K%d  Koppelrelais 24 V DC, 1 W, 6 A" % (n + 1),
              "     (z.B. Finder 38.51.7.024.0050)",
              "-M1…M%d  therm. Stellantrieb 230 V NC" % n,
              "-X2  Reihenklemmen Antriebe, -X3 Anforderung",
              "- - -  Feldleitung außerhalb der UV"]
    for j, z in enumerate(zeilen):
        s.text(lx + (j // 5) * 420, ly + 20 + (j % 5) * 15, z, 10)
    s.speichern("stromlaufplan.svg")


# --------------------------------------------------------------- Systemübersicht
def systemuebersicht():
    s = SVG(1200, 640)
    s.text(20, 30, "Systemübersicht Fußbodenheizungsregelung", 18, fett=True)

    raeume = ["Wohnen", "Küche", "Bad", "Schlafen", "Kind", "Büro"]
    for i, r in enumerate(raeume):
        x = 30 + i * 190
        s.rechteck(x, 60, 160, 90, fuell="#f4f8ff")
        s.text(x + 80, 82, "Raumsensor " + r, 11, "middle", fett=True)
        s.text(x + 80, 100, "ESP32-C3 + SHT40", 10, "middle")
        s.text(x + 80, 116, "5-V-USB-Netzteil", 10, "middle")
        s.text(x + 80, 132, "Montage 1,5 m, Innenwand", 9, "middle")
        s.linie(x + 80, 150, 600, 250, w=1.2, strich="7 5", farbe="#1a5fb4")
    s.text(30, 200, "ESP-NOW 2,4 GHz", 12, farbe="#1a5fb4", fett=True)
    s.text(30, 216, "(Funk, ohne Router nutzbar)", 11, farbe="#1a5fb4")

    # WLAN-Router optional
    s.rechteck(930, 200, 220, 60, strich="5 4", fuell="#fafafa")
    s.text(1040, 225, "WLAN-Router (optional)", 11, "middle", fett=True)
    s.text(1040, 243, "nur Uhrzeit (NTP) für Absenkung", 9, "middle")
    s.linie(930, 230, 820, 300, w=1.2, strich="5 4", farbe="#666")

    # UV
    s.rechteck(380, 250, 440, 230, w=2, fuell="#fffaf0")
    s.text(390, 272, "Unterverteilung (UV)", 13, fett=True)
    box = [("-Q1 LS B6", 400, 290), ("-T1 Netzteil 24 V", 520, 290), ("-U1 DC/DC 5 V", 660, 290),
           ("-A1 ESP32 Zentrale", 400, 350), ("-A2/A3 ULN2803A", 560, 350),
           ("-K1…K9 Koppelrelais", 400, 410), ("-X2/-X3 Reihenklemmen", 590, 410)]
    for t, x, y in box:
        b = 150 if "Koppel" in t or "Reihen" in t or "ESP32" in t else 120
        s.rechteck(x, y, b, 44, fuell="#fff")
        s.text(x + b / 2.0, y + 27, t, 10, "middle")
    s.text(810, 470, "ext. Antenne", 9, "end")

    # Heizkreisverteiler
    s.rechteck(380, 540, 440, 80, fuell="#f3fff3")
    s.text(390, 562, "Heizkreisverteiler (Fußbodenheizung)", 13, fett=True)
    for i in range(8):
        x = 400 + i * 52
        s.rechteck(x, 572, 40, 36, fuell="#fff")
        s.text(x + 20, 595, "M%d" % (i + 1), 10, "middle")
    s.linie(600, 480, 600, 540, w=2.5)
    s.text(610, 510, "Feldleitung 230 V, z.B. 2 × NYM-J 7×1,5 mm²", 10)

    # Wärmeerzeuger
    s.rechteck(900, 400, 260, 70, fuell="#fafafa", strich="5 4")
    s.text(1030, 425, "Wärmeerzeuger / Heizkreispumpe", 11, "middle", fett=True)
    s.text(1030, 445, "Eingang Wärmeanforderung", 9, "middle")
    s.text(1030, 458, "(optional, potentialfrei über K9)", 9, "middle")
    s.linie(820, 435, 900, 435, w=1.2, strich="5 4")
    s.speichern("systemuebersicht.svg")


# --------------------------------------------------------------- Hutschiene
def hutschiene():
    te = 17.5          # mm je Teilungseinheit
    f = 3.2            # px je mm
    s = SVG(int(12 * te * f) + 220, 470)
    s.text(20, 30, "Hutschienenbelegung (Vorschlag, 2 Reihen à 12 TE, 1 TE = 17,5 mm)", 16, fett=True)
    reihen = [
        [("-Q1", "LS B6", 17.5), ("-T1", "HDR-15-24", 17.5), ("-A1/A2/A3/U1", "Gehäuse 6 TE (Träger-", 105.0),
         ("", "", 0), ("frei", "", 70.0)],
        [("-K1…K9", "9 × 6,2 mm", 55.8), ("-X2", "8×L 8×N 1×PE", 17 * 5.2),
         ("-X3", "2 Kl.", 2 * 5.2), ("Endw.", "", 3 * 5.2), ("frei", "", 0)],
    ]
    for r, reihe in enumerate(reihen):
        y = 70 + r * 190
        x = 40
        s.rechteck(x - 10, y + 50, 12 * te * f + 20, 18, fuell="#ddd", w=0.8)
        s.text(x - 10, y - 8, "Reihe %d" % (r + 1), 12, fett=True)
        for name, info, mm in reihe:
            if mm <= 0:
                continue
            b = mm * f
            frei = name == "frei"
            s.rechteck(x, y, b, 120, fuell="#fff" if frei else "#eef4ff", strich="4 3" if frei else None)
            s.text(x + b / 2, y + 30, name, 11, "middle", fett=not frei)
            if "Träger" in info:
                s.text(x + b / 2, y + 48, "Gehäuse 6 TE", 9, "middle")
                s.text(x + b / 2, y + 62, "Trägerplatine:", 9, "middle")
                s.text(x + b / 2, y + 76, "ESP32, ULN2803A,", 9, "middle")
                s.text(x + b / 2, y + 90, "DC/DC, Pulldowns", 9, "middle")
            elif b < 60:
                s.text(x + b / 2 + 3, y + 78, info, 9, "middle", dreh=-90)
            else:
                s.text(x + b / 2, y + 50, info, 9, "middle")
            s.text(x + b / 2, y + 140, "%.1f mm" % mm, 9, "middle")
            x += b
    s.text(40, 450, "Antennenkabel (U.FL → SMA) aus der UV führen, falls Gehäuse/Abdeckung aus Metall.",
           11)
    s.speichern("hutschiene.svg")


if __name__ == "__main__":
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 8
    stromlaufplan(n)
    systemuebersicht()
    hutschiene()
