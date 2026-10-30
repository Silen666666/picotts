# Fotobox mit Sony Alpha 6000 (ILCE-6000)

## Konzept

```
 [Fernbedienung / Buzzer]          [Tablet: Browser, Vollbild]
   Bluetooth-Presenter                ^  zeigt Countdown + Foto
   oder GPIO-Taster                   |  "Foto!"-Knopf löst ebenfalls aus
          |                           |  WLAN (http://<rechner>:8000)
          v                           |
 [Raspberry Pi / Linux-PC]  ----------+
   fotobox.py: Countdown -> Auslösen -> Speichern -> Bild ans Tablet
          |
          | USB-Kabel (Kamera: USB-Verbindung = "PC-Fernbedienung")
          v
 [Sony a6000]  mit Netzteil-Akkuattrappe (AC-PW20 oder Nachbau)
```

* **Auslösen**: per Fernbedienung, Buzzer, Enter-Taste oder Knopf auf dem Tablet.
* **Speichern**: Originalbild in `~/fotobox_bilder/`, verkleinerte Kopie (1920 px) in `~/fotobox_bilder/web/`.
* **Anzeigen**: Das Tablet öffnet `http://<IP-des-Rechners>:8000`. Neue Bilder erscheinen dort sofort (Server-Sent Events), ohne Neuladen.

## Installation (Raspberry Pi OS / Debian / Ubuntu)

```bash
sudo apt install gphoto2 python3-pil      # Pflicht: gphoto2; Pillow empfohlen
pip install gphoto2                       # optional: schnellere, dauerhafte Kameraverbindung
pip install evdev                         # optional: Bluetooth-Fernbedienung
# gpiozero ist auf Raspberry Pi OS schon dabei (für Taster am GPIO)
```

Auf Desktop-Systemen belegt oft der Dateimanager (gvfs) die Kamera. Dann:

```bash
pkill -f gvfs-gphoto2-volume-monitor; pkill -f gvfsd-gphoto2
```

Testen, ob die Kamera erkannt wird:

```bash
gphoto2 --auto-detect           # sollte "Sony Alpha-A6000 (Control)" zeigen
gphoto2 --capture-image-and-download
```

## Starten

```bash
python3 fotobox.py --simulate                       # Test ohne Kamera
python3 fotobox.py                                  # Kamera per USB
python3 fotobox.py --evdev /dev/input/event3 --keys KEY_VOLUMEUP,KEY_ENTER
python3 fotobox.py --gpio 17 --countdown 5          # Buzzer zwischen GPIO17 und GND
```

Welches `/dev/input/eventX` die Fernbedienung ist, zeigt `python3 -m evdev.evtest`.
Günstige Bluetooth-Selfie-Fernbedienungen senden meist `KEY_VOLUMEUP`, Presenter oft `KEY_PAGEDOWN`/`KEY_RIGHT`. Das musst du an deinem Gerät nachsehen.

Auf dem Tablet die Seite im Vollbild öffnen (Android: Chrome → „Zum Startbildschirm hinzufügen“; iPad: Safari → „Zum Home-Bildschirm“) und das automatische Sperren des Displays ausschalten.

## Kamera-Einstellungen gegen das Langsamwerden

| Einstellung | Empfehlung | Warum |
|---|---|---|
| Stromversorgung | Netzteil mit Akkuattrappe (AC-PW20 o. Nachbau) | kein leerer Akku nach wenigen Stunden |
| USB-Verbindung | PC-Fernbedienung | Auslösen per gphoto2 |
| Dateiformat | nur JPEG, ohne RAW | kleinere Dateien, schneller übertragen |
| Bildgröße | M (12 MP) reicht für Tablet und Druck bis ca. 20×30 cm | halbe Datenmenge |
| Auto-Review | Aus | Kamera ist schneller wieder bereit |
| Langzeit-RM, Multiframe-RM, Auto HDR, Bildeffekte | Aus | brauchen zusätzliche Rechenzeit oder Aufnahmen |
| Flugmodus | Ein | WLAN/NFC aus |
| Speicherkarte | UHS-I U3, in der Kamera formatieren | siehe unten |
| Fokus | AF-S, oder MF mit festem Abstand | Auslösen schlägt nicht fehl, wenn der AF nichts findet |
