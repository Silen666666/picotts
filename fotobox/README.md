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

## Installation unter Windows (z.B. Windows-Tablet)

**Automatisch:** `install_fotobox.bat` doppelklicken. Das Skript installiert Python, MSYS2, gphoto2
und Pillow und führt dich durch den Treiberwechsel mit Zadig. Die manuellen Schritte:

Kamera per USB direkt am Tablet, die Fotobox-Seite läuft im Browser desselben Tablets
(`http://localhost:8000`). Ein Android-Tablet kann zusätzlich als zweite Anzeige über WLAN dienen.

1. **Python** von https://www.python.org/downloads/ installieren, dabei „Add python.exe to PATH“ anhaken.
   Optional: `py -m pip install pillow` (verkleinerte Bilder).
2. **MSYS2** von https://www.msys2.org/ installieren (Standardordner `C:\msys64`), dann im Startmenü
   „MSYS2 UCRT64“ öffnen und eingeben:
   ```
   pacman -Syu
   pacman -S mingw-w64-ucrt-x86_64-gphoto2
   ```
   `fotobox.py` findet `C:\msys64\ucrt64\bin\gphoto2.exe` automatisch.
3. **USB-Treiber**: libgphoto2 funktioniert nicht mit dem Standard-Windows-Treiber, es braucht einen
   libusb-kompatiblen Treiber. Mit **Zadig** (https://zadig.akeo.ie/):
   Kamera einschalten, USB-Verbindung = **PC-Fernbedienung**, anstecken → Zadig → *Options → List All Devices*
   → Sony-Kamera wählen → Treiber **WinUSB** → *Replace Driver*.
   Hinweis: Im Modus PC-Fernbedienung meldet sich die a6000 mit einer anderen USB-Kennung (054C:094E)
   als im Massenspeicher-/MTP-Modus (054C:08B7) – laut libgphoto2-Quellcode. Der Treiberwechsel
   betrifft daher nur den Fernbedienungsmodus.
4. Test in der „MSYS2 UCRT64“-Konsole: `gphoto2 --auto-detect` und `gphoto2 --capture-image-and-download`.
5. Den Ordner `fotobox` herunterladen und `start_fotobox.bat` doppelklicken
   (zum Testen ohne Kamera in einer Eingabeaufforderung: `start_fotobox.bat --simulate`).
   Bilder landen in `Bilder\Fotobox`. Fragt die Windows-Firewall nach, Zugriff im **privaten** Netzwerk erlauben,
   damit ein zweites Tablet die Seite über WLAN öffnen kann.
6. Browser mit **F11** in den Vollbildmodus schalten.

**Fernbedienung unter Windows:** Bluetooth-Presenter oder Buzzer, die sich als Tastatur melden, lösen direkt
in der Browser-Seite aus. Standardmäßig reagieren Enter, Leertaste, Bild↑/Bild↓, Pfeil links/rechts und
Lauter-Taste (`--browser-keys` ändert das). Die Browser-Seite muss dafür im Vordergrund sein.
Ob eine Selfie-Fernbedienung die Lauter-Taste an den Browser durchreicht, ist gerätabhängig –
ein Presenter (Bild↓) ist die sicherere Wahl.

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

## Kamera wird nicht erkannt (`gphoto2 --auto-detect` ist leer)

1. USB-Verbindung im Kameramenü auf **PC-Fernbedienung** stellen (nicht Auto/MTP/Massenspeicher), Kamera **einschalten**.
2. Echtes **Datenkabel** verwenden – viele Micro-USB-Kabel laden nur.
3. **Treiber prüfen**: Im Geräte-Manager muss die Kamera nach dem Zadig-Schritt unter
   „Universal Serial Bus devices“ mit Treiber WinUSB stehen. Hat sich Zadig noch das MTP-Gerät
   gegriffen, Kamera erst auf PC-Fernbedienung stellen, neu anstecken und Zadig wiederholen.
4. Den Befehl in der Konsole **„MSYS2 UCRT64“** ausführen, nicht in „MSYS2 MSYS“.
5. Mehr Details: `gphoto2 --list-ports` und `gphoto2 --debug --auto-detect`.
