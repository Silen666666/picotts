# ESP CTF Game (v2.0.0)

Mehrspieler-Bewegungsspiel mit ESP32-„Nodes“ (LED-Leiste + Taster) und einem
ESP32-„Master“, der ein eigenes WLAN aufspannt und über den Handy-Browser
bedient wird. 12 Spiele, darunter Capture the Flag, Reaktion, Simon Says und
Whack-a-Mole.

Ab v2.0 können **Master und Nodes kabellos aktualisiert werden (OTA)**:
Firmware einmal im Browser hochladen, und der Master verteilt sie an alle Nodes.

---

## Hardware

| Teil | Hinweis |
|---|---|
| ESP32-WROOM-32 „NodeMCU“ (30 Pin) | 1× Master + 1× je Node |
| WS2812B-8 LED-Leiste | an **D13** (über 300 Ω) |
| Taster | an **D18** und GND (kein Widerstand nötig) |
| USB-Netzteil / Powerbank 5 V, mind. 1 A | je Node |

Verdrahtung im Detail: [VERDRAHTUNG.txt](VERDRAHTUNG.txt)

> **Maximal 9 Nodes + 1 Handy.** Der WLAN-Access-Point des ESP32 lässt
> höchstens 10 Geräte gleichzeitig zu. Eine 10. Node bekommt keine Verbindung
> (blinkt gelb).

---

## Software einrichten (einmalig)

1. **Arduino IDE 2.x** installieren.
2. Boardverwalter: **„esp32 by Espressif Systems“** installieren
   (getestet mit **3.3.8**; jede 3.3.x sollte gehen).
3. Bibliotheksverwalter: **„Adafruit NeoPixel“** installieren (mind. 1.12.4).
4. Unter *Werkzeuge* einstellen (Master **und** Nodes):
   - Board: **ESP32 Dev Module**
   - Partition Scheme: **Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS)**
     – wichtig: nur damit funktionieren OTA-Updates und das Speichern der
     Node-Firmware auf dem Master.

### Erstes Flashen (per USB)

1. `master/master.ino` öffnen → Master anschließen → **Hochladen**.
2. `node/node.ino` öffnen → jede Node anschließen → **Hochladen**.
   Alle Nodes bekommen dieselbe Firmware; die Nummern vergibt der Master
   automatisch und merkt sie sich (pro Node fest, auch nach Neustarts).

> Nodes mit alter Firmware (1.x) müssen **einmal** per USB auf v2.0 gebracht
> werden. Danach geht alles per OTA. Das Web-UI zeigt alte Nodes als Warnung an.

---

## Bedienung

1. Master einschalten, Nodes einschalten.
2. Handy mit dem WLAN **„ESP-CTF-Game“** verbinden (ohne Passwort).
   Tipp: mobile Daten kurz ausschalten, sonst „flüchtet“ manches Handy ins Mobilnetz.
3. Browser: **http://192.168.4.1**

Im Web-UI:
- **Status**: welche Nodes verbunden sind, mit **Signalstärke** (dBm) und
  Firmware-Version. **„Finden“** lässt eine Node 5 s weiß blinken, so kann man
  Nummer und Gerät zuordnen.
- **Spieler-Namen** je Node (werden gespeichert).
- **Einstellungen**: Spiel wählen, Optionen, **STARTEN / STOPPEN**.
- **NODES RECONNECT**: alle Nodes melden sich neu an (Nummern bleiben).
- **Node-Nummern neu vergeben**: Nummern zurücksetzen (neue Reihenfolge).
- **Firmware & OTA**: Updates (siehe unten).

### Was bedeuten die LEDs einer Node?

| LED | Bedeutung |
|---|---|
| gelb blinkend | sucht WLAN / WLAN-Verbindung verloren |
| blau langsam blinkend | WLAN ok, aber (noch) nicht beim Master angemeldet |
| kurzer grüner Blitz, dann grün | angemeldet, bereit |
| weiß schnell blinkend | „Finden“ wurde im Web-UI gedrückt |
| lila Fortschrittsbalken | Firmware-Update läuft |
| rot schnell blinkend (2 s) | Firmware-Update fehlgeschlagen (alte Firmware läuft weiter) |

---

## OTA-Updates (kabellos)

### 1. Firmware-Datei erzeugen

In der Arduino IDE: **Sketch → Kompilierte Binärdatei exportieren**.
Danach liegt im Sketch-Ordner unter `build/esp32.esp32.esp32/`:

- `node.ino.bin` (bzw. `master.ino.bin`) → **diese Datei verwenden**
- *nicht* verwenden: `…merged.bin`, `…bootloader.bin`, `…partitions.bin`

### 2. Nodes aktualisieren

Web-UI → **Firmware & OTA** → *Node-Firmware hochladen* → `node.ino.bin` wählen.
Dann **„Veraltete Nodes aktualisieren“**. Der Master schickt die Firmware
nacheinander an jede Node (je ca. 15–30 s); der Fortschritt steht im Web-UI.
Spiele sind währenddessen gesperrt.

### 3. Master aktualisieren

Web-UI → **Firmware & OTA** → *Master-Firmware hochladen* → `master.ino.bin`.
Der Master startet danach neu (Handy verbindet sich wieder, Seite neu laden).

### Sicherheitsnetz

- Der Master prüft jede hochgeladene Datei: falsche Datei (z. B. Master-Firmware
  als Node-Firmware, `merged.bin`, `bootloader.bin`) und unpassende
  Protokollversionen werden mit einer Meldung abgelehnt.
- Die Nodes prüfen Prüfsumme (MD5) und Image, bevor sie umschalten.
- **Automatischer Rückfall (Rollback):** Startet eine neue Firmware nicht richtig
  (Absturz, bevor sie sich beim Master angemeldet hat), schaltet der ESP32 beim
  nächsten Start automatisch auf die vorherige Firmware zurück.

### Alternative: Arduino IDE über WLAN

PC mit „ESP-CTF-Game“ verbinden → *Werkzeuge → Port* → Netzwerk-Port
`ctf-master` bzw. `ctf-node-xxxxxx` wählen → Hochladen.
(Optional mit Passwort: `OTA_PASSWORD` in `config.h`.)

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
| 11 | Farbjagd | Die erste Node zeigt die Zielfarbe; die Node mit dieser Farbe finden und drücken. |
| 12 | Whack-a-Mole | Gelbe Node schnell treffen. |

Bei Reaktion, Whack-a-Mole und Knockout gibt es einen einstellbaren
**Rundenabstand** (Standard 10 s), damit niemand einfach dem Gewinner hinterherläuft.

Es spielen immer die Nodes mit, die **beim Start verbunden** sind.

---

## Einstellungen (`config.h`)

**master/config.h**

| Option | Standard | Bedeutung |
|---|---|---|
| `WIFI_SSID` / `WIFI_PASS` | `ESP-CTF-Game` / leer | WLAN des Masters (Passwort mind. 8 Zeichen oder leer) |
| `WIFI_CHANNEL` | `0` | 0 = beim Start freiesten Kanal (1/6/11) wählen, sonst fester Kanal |
| `AP_MAX_CONN` | `10` | max. WLAN-Geräte (ESP32: höchstens 10) |
| `MAX_NODES` | `16` | gespeicherte Node-Nummern |
| `OTA_PASSWORD` | leer | Passwort für Arduino-IDE-Update über WLAN |
| `USE_WATCHDOG` / `WDT_TIMEOUT_S` | `1` / `30` | Neustart, falls die Firmware hängt |
| `CTF_TEAMS`, `CTF_DURATION_S`, `ROUND_DELAY_S`, … | | Spielvorgaben (auch im Web-UI einstellbar) |

**node/config.h**: Pins (`NEO_PIN` = 13, `BUTTON_PIN` = 18), `NEO_COUNT`,
WLAN-Name (muss zum Master passen), `OTA_PASSWORD`, `USE_WATCHDOG`.

Ältere `config.h`-Kopien funktionieren weiter – fehlende Optionen bekommen
automatisch Standardwerte.

---

## Fehlersuche

| Problem | Ursache / Lösung |
|---|---|
| Node reagiert träge / fällt aus | Signalstärke im Web-UI prüfen (schlechter als ca. −75 dBm = zu weit weg / abgeschirmt). |
| Web-UI zeigt „Brownout“ bei einer Node | Stromversorgung zu schwach (Kabel, Powerbank) – führt zu Neustarts. |
| Node blinkt dauerhaft gelb | WLAN nicht gefunden oder Access Point voll (max. 9 Nodes + Handy). |
| Node blinkt dauerhaft blau | Master nicht erreichbar; „NODES RECONNECT“ drücken. |
| Warnung „alte Firmware 1.x“ | Diese Node einmal per USB mit `node/node.ino` flashen. |
| Upload „Datei passt nicht“ | Falsche Datei gewählt – `node.ino.bin` bzw. `master.ino.bin` aus `build/esp32.esp32.esp32/` verwenden. |
| „Dateisystem nicht verfügbar“ | Partition Scheme „Default 4MB with spiffs“ wählen und Master neu flashen. |
| Seriell (115200 Baud) | Master: `1`–`9`, `k`, `h`, `w` = Spiel starten, `0` = Stopp, `s` = Status, `r` = Reconnect. |

---

## Was ist neu in v2.0.0?

- **OTA**: Node-Firmware einmal hochladen, der Master verteilt sie; Master-Update im Browser;
  Dateiprüfung + automatischer Rollback.
- **Stabilität**:
  - LED-Befehle und Tastendrücke werden bestätigt und bei Verlust wiederholt.
  - Node-Nummern hängen fest an der Hardware (MAC) statt an der IP. Kein „Geister-Node“ mehr nach IP-Wechsel.
  - Größerer Empfangspuffer auf dem Master.
  - WLAN-Aussetzer kosten keine Anmeldung mehr; eine neu gestartete Node bekommt ihre Spielfarbe zurück.
  - Watchdog auf Master und Nodes.
  - Automatische Kanalwahl.
- **Spiele**: Ziele und Folgen nur noch auf verbundenen Nodes (keine unlösbaren Runden),
  keine hängenden Runden mehr, CTF zeigt „gesperrt“ jetzt sichtbar an.
- **Web-UI**: Signalstärke, Firmware-Version, Neustart-Grund (z. B. Brownout) je Node,
  „Finden“-Knopf, Warnhinweise, Firmware-Karte.
