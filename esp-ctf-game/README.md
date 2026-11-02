# ESP CTF Game (v3.0.0)

Mehrspieler-Bewegungsspiel mit ESP32-„Nodes“ (LED-Leiste + Taster) und einem
ESP32-„Master“, der über den Handy-Browser bedient wird. 12 Spiele, darunter
Capture the Flag, Reaktion, Simon Says und Whack-a-Mole.

**Neu in v3:**
- Die Nodes funken per **ESP-NOW** mit dem Master statt über das WLAN.
  Dadurch sind **bis zu 19 Nodes** möglich (vorher 9). Die Reaktion ist schneller,
  und es gibt kein Wiederverbinden mit dem WLAN mehr.
- **Ein-Klick-Update**: Master- und Node-Firmware zusammen im Browser hochladen.
  Der Master aktualisiert sich und verteilt danach automatisch an alle Nodes.

---

## Hardware

| Teil | Hinweis |
|---|---|
| ESP32-WROOM-32 „NodeMCU“ (30 Pin) | 1× Master + 1× je Node |
| WS2812B-8 LED-Leiste | an **D13** (über 300 Ω) |
| Taster | an **D18** und GND (kein Widerstand nötig) |
| USB-Netzteil / Powerbank 5 V, mind. 1 A | je Gerät |

Verdrahtung im Detail: [VERDRAHTUNG.txt](VERDRAHTUNG.txt)

---

## Software einrichten (einmalig)

1. **Arduino IDE 2.x** installieren.
2. Boardverwalter: **„esp32 by Espressif Systems“** installieren (Version 3.3.x).
3. Bibliotheksverwalter: **„Adafruit NeoPixel“** installieren (mind. 1.12.4).
4. Unter *Werkzeuge* einstellen (Master **und** Nodes):
   - Board: **ESP32 Dev Module**
   - Partition Scheme: **Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS)**.
     Nur damit funktionieren Funk-Updates und das Speichern der Node-Firmware auf
     dem Master. Bei Boards ohne diesen Menüpunkt (z. B. „DOIT ESP32 DEVKIT V1“) ist
     diese Aufteilung automatisch eingestellt.

### Ordner – wichtig!

```
esp-ctf-game\
  master\  master.ino, webui.h, config.h, protocol.h   ← alle 4 Dateien zusammen
  node\    node.ino, config.h, protocol.h               ← alle 3 Dateien zusammen
```
Der Ordnername muss wie die `.ino`-Datei heißen. `webui.h` enthält die
Web-Oberfläche und muss neben `master.ino` liegen.

### Erstes Flashen (per USB – nur einmal nötig)

1. `master/master.ino` öffnen → Master anschließen → **Hochladen**.
2. `node/node.ino` öffnen → jede Node anschließen → **Hochladen**.
   Alle Nodes bekommen dieselbe Firmware; die Nummern vergibt der Master
   automatisch und merkt sie sich (pro Node fest, auch nach Neustarts).

> **Umstieg von v1/v2:** v3 funkt anders (ESP-NOW). Master **und alle Nodes**
> müssen deshalb einmal per USB auf v3 gebracht werden. Danach gehen **alle
> künftigen Updates per Funk**, auch solche, die das Spiel-Protokoll ändern.
> Noch nicht umgestellte v1/v2-Nodes zeigt das Web-UI als Warnung an.

---

## Bedienung

1. Master einschalten, dann die Nodes.
2. Handy mit dem WLAN **„ESP-CTF-Game“** verbinden (ohne Passwort).
   Tipp: mobile Daten kurz ausschalten, sonst „flüchtet“ manches Handy ins Mobilnetz.
3. Browser: **http://192.168.4.1**

Im Web-UI:
- **Status**: welche Nodes verbunden sind, mit **Signalstärke** (dBm) und
  Firmware-Version. **„Finden“** lässt eine Node 5 s weiß blinken, so kann man
  Nummer und Gerät zuordnen.
- **Spieler-Namen** je Node (werden gespeichert).
- **Einstellungen → Spielmodus**: Das Feld zeigt anfangs „Capture the Flag“.
  **Antippen** öffnet die Liste aller 12 Spiele. Darunter stehen die Optionen und
  **STARTEN / STOPPEN**.
- **NODES RECONNECT**: alle Nodes melden sich neu an (Nummern bleiben).
- **Node-Nummern neu vergeben**: Nummern zurücksetzen (neue Reihenfolge).
- **WLAN-Kanal neu wählen**: Der Master sucht den freiesten Funkkanal neu. Das ist
  nur nötig, wenn es in der Umgebung viele WLANs gibt und Nodes oft aussetzen.
- **Firmware & OTA**: Updates (siehe unten).

### Was bedeuten die LEDs einer Node?

| LED | Bedeutung |
|---|---|
| gelb langsam blinkend | sucht den Master (Master aus, zu weit weg, oder WLAN-Name in `config.h` ≠ Master) |
| blau langsam blinkend | Master gefunden, wartet auf Anmeldung |
| kurzer grüner Blitz, dann grün | angemeldet, bereit |
| lila langsam blinkend | **Update nötig**: Node und Master haben verschiedene Protokolle. Im Web-UI „Alles aktualisieren“ bzw. „Veraltete Nodes aktualisieren“ |
| weiß schnell blinkend | „Finden“ wurde im Web-UI gedrückt |
| lila Fortschrittsbalken | Firmware-Update läuft |
| rot schnell blinkend (2 s) | Firmware-Update fehlgeschlagen (alte Firmware läuft weiter) |

---

## Updates per Funk (OTA)

### 1. Firmware-Dateien erzeugen

In der Arduino IDE (je einmal für `master.ino` und `node.ino`):
**Sketch → Kompilierte Binärdatei exportieren**. Danach liegt im Sketch-Ordner
unter `build/esp32.esp32.esp32/`:

- `master.ino.bin` bzw. `node.ino.bin` → **diese Dateien verwenden**
- *nicht* verwenden: `…merged.bin`, `…bootloader.bin`, `…partitions.bin`

Fertig gebaute Dateien liegen auch im Ordner `firmware/` des ZIP-Pakets.

### 2. Ein-Klick-Update (empfohlen)

Web-UI → **Firmware & OTA** → **Alles aktualisieren**:
`master.ino.bin` und `node.ino.bin` auswählen und einmal klicken. Eine der beiden
Dateien reicht ebenfalls.

1. Der Master speichert die Node-Firmware und aktualisiert sich selbst.
2. Er startet neu. Das Handy verbindet sich wieder, die Seite lädt neu.
3. Etwa 25 s später verteilt er die Firmware **automatisch** nacheinander an alle
   Nodes (je ca. 15–30 s). Der Fortschritt steht im Web-UI.

Spiele sind währenddessen gesperrt. Zum Laden verbindet sich jede Node kurz mit dem
Master-WLAN. Das geschieht nacheinander, die 10-Geräte-Grenze des WLANs stört daher nicht.

### 3. Einzeln (für Fortgeschrittene)

Unter **Firmware & OTA** gibt es weiterhin die getrennten Knöpfe:
- „Master-Firmware hochladen“ und „Node-Firmware hochladen“
- „Veraltete Nodes aktualisieren“ und „Alle Nodes neu flashen“

### Sicherheitsnetz

- Der Master prüft jede hochgeladene Datei. Falsche Dateien werden mit einer Meldung
  abgelehnt, etwa Master-Firmware als Node-Firmware, `merged.bin` oder `bootloader.bin`.
- Die Nodes prüfen Prüfsumme (MD5) und Image, bevor sie umschalten.
- **Automatischer Rückfall (Rollback):** Startet eine neue Firmware nicht richtig
  (Absturz, bevor sie sich beim Master angemeldet hat), schaltet der ESP32 beim
  nächsten Start automatisch auf die vorherige Firmware zurück.
- **Update-Modus:** Passen Master und Node nicht zusammen (verschiedene
  Protokolle), blinkt die Node lila. Sie spielt dann nicht mit, lässt sich aber
  weiter per Funk aktualisieren.

### Master über die Arduino IDE per WLAN

PC mit „ESP-CTF-Game“ verbinden → *Werkzeuge → Port* → Netzwerk-Port
`ctf-master` wählen → Hochladen (optional mit `OTA_PASSWORD` in `config.h`).
Die Nodes sind in v3 nicht mehr im WLAN und werden über den Master aktualisiert.

---

## Die Spiele

| # | Spiel | Kurzregel |
|---|---|---|
| 1 | Capture the Flag | Node drücken = für dein Team einnehmen (Farbe wechselt). Danach ist die Node für die eingestellte **Sperrdauer** gesperrt (schrumpfender Balken, zu früh drücken = orange). Nach Ablauf der Zeit gewinnt das Team mit den meisten Nodes. |
| 2 | Memory | Nodes zeigen 2 s ihre Farbe. Zwei gleichfarbige nacheinander drücken = Paar. |
| 3 | Bombenentschärfung | Rot blinkend = Bombe. Die gezeigte Reihenfolge nachdrücken. Bombe drücken = Reihenfolge nochmal zeigen. |
| 4 | Reaktion | Eine Node leuchtet gelb – schnell drücken. Reaktionszeit + Bestenliste. |
| 5 | Simon Says | Jede Node hat eine feste Farbe. Gezeigte Folge nachdrücken; jede Runde einen Schritt länger. |
| 6 | Heiße Kartoffel | Orange blinkend = du hast die Kartoffel – drücken gibt sie weiter. Wer sie beim Platzen hält, verliert ein Leben. |
| 7 | King of the Hill | Goldene Node = Thron. Drücken = halten. Der Thron wandert alle 30 s. |
| 8 | Tauziehen | Erste Hälfte der Nodes = ROT, Rest = BLAU. Jeder Druck zieht den Balken. |
| 9 | Minesweeper | Manche Nodes sind Minen. Alle sicheren finden, 3 Leben. |
| 10 | Knockout | Wie Reaktion – wer nach dem ersten Treffer nicht innerhalb der Gnadenfrist drückt, verliert ein Leben. |
| 11 | Farbjagd | Eine Node zeigt die Zielfarbe; die Node mit dieser Farbe finden und drücken. |
| 12 | Whack-a-Mole | Gelbe Node schnell treffen. |

Bei Reaktion, Whack-a-Mole und Knockout gibt es einen einstellbaren
**Rundenabstand** (Standard 10 s), damit niemand einfach dem Gewinner hinterherläuft.

Es spielen immer die Nodes mit, die **beim Start verbunden** sind.

---

## Option: Master als Spieler-Station (Master-Button)

Der Master kann zusätzlich eine eigene LED-Leiste und einen Taster bekommen und
spielt dann wie eine Node mit. Das ist **standardmäßig aus**; ausgeschaltet ist die
Firmware exakt wie ohne diese Funktion.

Einschalten: in `master/config.h` `#define MASTER_IS_NODE 1` setzen, neu flashen.

- Verdrahtung wie bei einer Node: LED-Leiste DIN über 300 Ω an **D13**, 5 V an
  **VIN**, GND. Taster zwischen **D18** und GND, 100 nF parallel zum Taster.
- **Strom ist wichtig.** Fällt die Spannung am Master ab (Brownout), endet das Spiel
  für alle. Daher gilt:
  - Netzteil mit mind. 1 A und kurzes, dickes Kabel mit Zugentlastung.
  - 470–1000 µF Kondensator an der LED-Leiste.
  - Die LEDs sind am Master bewusst gedimmt (`MASTER_NEO_BRIGHTNESS 45`).
- **Am besten** Taster und LED-Leiste in ein eigenes Kästchen an 1–2 m Kabel setzen.
  So schlagen Spieler nicht auf den Master, und er kann zentral stehen bleiben.
- Fairness: Der Master-Taster hat keinen Funkweg. Er wird deshalb 6 ms verzögert
  gewertet (`MASTER_LAT_MS`). Bei der Farbjagd ist er nie die Anzeige-Node.
- Der Master-Spieler belegt eine der Node-Nummern (`MAX_NODES`).

---

## Einstellungen (`config.h`)

**master/config.h**

| Option | Standard | Bedeutung |
|---|---|---|
| `WIFI_SSID` / `WIFI_PASS` | `ESP-CTF-Game` / leer | WLAN des Masters (Passwort mind. 8 Zeichen oder leer). **Muss in `node/config.h` gleich sein!** |
| `WIFI_CHANNEL` | `0` | 0 = beim ersten Start freiesten Kanal (1/6/11) wählen und merken, sonst fester Kanal |
| `AP_MAX_CONN` | `10` | max. WLAN-Geräte (Handys + je eine Node während ihres Updates) |
| `MAX_NODES` | `16` | gespeicherte Node-Nummern (2–19) |
| `MASTER_IS_NODE` | `0` | 1 = Master ist zusätzlich Spieler-Station (siehe oben) |
| `OTA_PASSWORD` | leer | Passwort für Arduino-IDE-Update des Masters über WLAN |
| `USE_WATCHDOG` / `WDT_TIMEOUT_S` | `1` / `30` | Neustart, falls die Firmware hängt |
| `CTF_TEAMS`, `CTF_DURATION_S`, `ROUND_DELAY_S`, … | | Spielvorgaben (auch im Web-UI einstellbar) |

**node/config.h**: Pins (`NEO_PIN` = 13, `BUTTON_PIN` = 18), `NEO_COUNT`,
`WIFI_SSID`/`WIFI_PASS` (müssen zum Master passen – damit findet die Node den
Master und lädt Updates), `USE_WATCHDOG`.

Ältere `config.h`-Kopien funktionieren weiter – fehlende Optionen bekommen
automatisch Standardwerte.

---

## Fehlersuche

| Problem | Ursache / Lösung |
|---|---|
| Compilerfehler „'function' does not name a type“ | `webui.h` fehlt im Ordner `master` – alle 4 Master-Dateien zusammen ablegen. |
| Nur „Capture the Flag“ sichtbar | Das Feld „Spielmodus“ antippen – es ist eine Auswahlliste mit allen 12 Spielen. |
| Node blinkt dauerhaft gelb | Master aus oder zu weit weg, oder `WIFI_SSID` in `node/config.h` passt nicht zum Master. |
| Node blinkt lila | Update nötig – im Web-UI aktualisieren. |
| Node reagiert träge / fällt aus | Signalstärke im Web-UI prüfen (schlechter als ca. −80 dBm = zu weit weg / abgeschirmt). Ggf. „WLAN-Kanal neu wählen“. |
| Web-UI zeigt „Brownout“ bei einer Node | Stromversorgung zu schwach (Kabel, Powerbank) – führt zu Neustarts. |
| Warnung „alte Firmware v1/v2“ | Diese Node einmal per USB mit `node/node.ino` (v3) flashen. |
| Upload „Datei passt nicht“ | Falsche Datei gewählt – `node.ino.bin` bzw. `master.ino.bin` aus `build/esp32.esp32.esp32/` verwenden. |
| „Dateisystem nicht verfügbar“ | Partition Scheme „Default 4MB with spiffs“ wählen und Master neu flashen. |
| Seriell (115200 Baud) | Master: `1`–`9`, `k`, `h`, `w` = Spiel starten, `0` = Stopp, `s` = Status, `r` = Reconnect. Nodes zeigen beim Start Version, MAC und Neustart-Grund. |

---

## Versionen

**v3.0.0**
- Funk per **ESP-NOW**: bis 19 Nodes, kein WLAN-Wiederverbinden der Nodes, schnellere
  Reaktion. Der Master merkt sich seinen Funkkanal; die Nodes finden ihn nach einem
  Neustart sofort wieder.
- **Ein-Klick-Update** mit automatischer Verteilung an alle Nodes.
- **Update-Modus** und eingefrorener Update-Weg: Ab v3 lassen sich alle künftigen
  Versionen per Funk einspielen.
- Erkennung alter v1/v2-Nodes, Signalstärke wird vom Master gemessen.
- Optional: **Master als Spieler-Station** (`MASTER_IS_NODE`).

**v2.0.1** – Web-Oberfläche in `master/webui.h` (behebt den Arduino-IDE-Fehler
„'function' does not name a type“).

**v2.0.0** – Erste Version mit Funk-Updates (OTA); stabilere Übertragung durch
Bestätigung und Wiederholung von LED-Befehlen und Tastendrücken; feste Node-Nummern
je Hardware; Watchdog; keine hängenden Spiele; Signalstärke und Neustart-Grund im
Web-UI.
