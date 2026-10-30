#!/usr/bin/env python3
"""
Fotobox fuer Sony Alpha 6000 (ILCE-6000).

Ablauf:
  Fernbedienung / Buzzer / Tablet-Button  ->  Countdown  ->  Kamera loest per USB aus
  ->  Bild wird auf dem Rechner (z.B. Raspberry Pi) gespeichert
  ->  Tablet zeigt das Bild sofort im Browser an (http://<rechner>:8000).

Kamera: USB-Verbindung im Kameramenue auf "PC-Fernbedienung" stellen.
Ausloesen ueber gphoto2 (python-gphoto2 falls installiert, sonst gphoto2-Kommandozeile).

Nur Python-Standardbibliothek noetig. Optional:
  Pillow      -> verkleinerte Bilder fuers Tablet (schnelleres Laden)
  gphoto2     -> (pip: gphoto2) dauerhafte Kameraverbindung, schnelleres Ausloesen
  evdev       -> Bluetooth-Presenter/Fernbedienung als Ausloeser
  gpiozero    -> Taster/Buzzer am Raspberry-Pi-GPIO als Ausloeser
"""

import argparse
import json
import os
import queue
import shutil
import subprocess
import sys
import threading
import time
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote

try:
    from PIL import Image, ImageDraw
except ImportError:
    Image = None

try:
    import gphoto2 as gp
except ImportError:
    gp = None


# ---------------------------------------------------------------- Kamera

class CameraError(Exception):
    pass


class Camera:
    """Loest die Kamera aus und speichert das JPEG unter `target`."""

    def __init__(self, simulate=False):
        self.simulate = simulate
        self._cam = None
        if not simulate and gp is None and shutil.which("gphoto2") is None:
            raise SystemExit("Weder python-gphoto2 noch das Programm 'gphoto2' gefunden. "
                             "Installation: sudo apt install gphoto2  (oder: pip install gphoto2)")

    def capture(self, target: Path):
        if self.simulate:
            self._fake_photo(target)
        elif gp is not None:
            self._capture_python(target)
        else:
            self._capture_cli(target)

    # Dauerhafte Verbindung: spart pro Bild die Initialisierung der Kamera.
    def _capture_python(self, target):
        for attempt in (1, 2):
            try:
                if self._cam is None:
                    self._cam = gp.Camera()
                    self._cam.init()
                path = self._cam.capture(gp.GP_CAPTURE_IMAGE)
                cam_file = self._cam.file_get(path.folder, path.name, gp.GP_FILE_TYPE_NORMAL)
                cam_file.save(str(target))
                return
            except gp.GPhoto2Error as exc:
                self.close()
                if attempt == 2:
                    raise CameraError(f"gphoto2: {exc}") from exc
                time.sleep(1)

    def _capture_cli(self, target):
        cmd = ["gphoto2", "--capture-image-and-download",
               "--filename", str(target), "--force-overwrite"]
        try:
            res = subprocess.run(cmd, capture_output=True, text=True, timeout=45)
        except subprocess.TimeoutExpired as exc:
            raise CameraError("Zeitueberschreitung beim Ausloesen") from exc
        if res.returncode != 0 or not target.exists():
            raise CameraError((res.stderr or res.stdout).strip()[-300:] or "unbekannter Fehler")

    def _fake_photo(self, target):
        time.sleep(1.0)
        if Image is None:
            raise CameraError("Simulation braucht Pillow (pip install pillow)")
        img = Image.new("RGB", (1600, 1067), (40, 60, 90))
        ImageDraw.Draw(img).text((60, 60), "SIMULATION " + target.stem, fill=(255, 255, 255))
        img.save(target, "JPEG", quality=85)

    def close(self):
        if self._cam is not None:
            try:
                self._cam.exit()
            except Exception:
                pass
            self._cam = None


# ---------------------------------------------------------------- Fotobox-Logik

class Booth:
    def __init__(self, camera, photo_dir: Path, countdown: int, web_size: int):
        self.camera = camera
        self.photo_dir = photo_dir
        self.web_dir = photo_dir / "web"
        self.web_dir.mkdir(parents=True, exist_ok=True)
        self.countdown = countdown
        self.web_size = web_size
        self.busy = threading.Lock()
        self.clients = []
        self.clients_lock = threading.Lock()

    # --- Ereignisse an alle Tablets (Server-Sent Events)
    def subscribe(self):
        q = queue.Queue()
        with self.clients_lock:
            self.clients.append(q)
        return q

    def unsubscribe(self, q):
        with self.clients_lock:
            if q in self.clients:
                self.clients.remove(q)

    def broadcast(self, **event):
        with self.clients_lock:
            for q in self.clients:
                q.put(event)

    # --- Bilder
    def photos(self, limit=30):
        files = sorted(self.web_dir.glob("*.jpg"), reverse=True)
        return [f.name for f in files[:limit]]

    def _make_web_copy(self, original: Path):
        """Verkleinerte Kopie fuers Tablet; ein 24-MP-JPEG laedt dort sonst spuerbar langsam."""
        web = self.web_dir / original.name
        if Image is None:
            shutil.copyfile(original, web)
            return web
        with Image.open(original) as img:
            img.thumbnail((self.web_size, self.web_size))
            img.save(web, "JPEG", quality=88)
        return web

    # --- Ausloesen
    def trigger(self, source="?"):
        """Startet Countdown + Aufnahme im Hintergrund. False, wenn gerade beschaeftigt."""
        if not self.busy.acquire(blocking=False):
            return False
        threading.Thread(target=self._run, args=(source,), daemon=True).start()
        return True

    def _run(self, source):
        try:
            print(f"[{datetime.now():%H:%M:%S}] Ausgeloest durch {source}")
            for n in range(self.countdown, 0, -1):
                self.broadcast(type="countdown", value=n)
                time.sleep(1)
            self.broadcast(type="capturing")
            target = self.photo_dir / f"foto_{datetime.now():%Y%m%d_%H%M%S}.jpg"
            t0 = time.monotonic()
            self.camera.capture(target)
            web = self._make_web_copy(target)
            print(f"  gespeichert: {target}  ({time.monotonic() - t0:.1f} s)")
            self.broadcast(type="photo", name=web.name)
        except Exception as exc:
            print(f"  FEHLER: {exc}", file=sys.stderr)
            self.broadcast(type="error", message=str(exc))
        finally:
            self.busy.release()


# ---------------------------------------------------------------- Ausloeser (Fernbedienungen)

def stdin_trigger(booth):
    """Enter-Taste im Terminal (auch: USB-Fernausloeser/Buzzer, die sich als Tastatur melden)."""
    for _ in sys.stdin:
        booth.trigger("Tastatur/Terminal")


def evdev_trigger(booth, device_path, keys):
    """Bluetooth-Presenter, Selfie-Fernbedienung o.ae. (Linux-Eingabegeraet)."""
    import evdev
    dev = evdev.InputDevice(device_path)
    print(f"Fernbedienung: {dev.name} ({device_path}), Tasten: {', '.join(keys) or 'alle'}")
    for ev in dev.read_loop():
        if ev.type == evdev.ecodes.EV_KEY and ev.value == 1:
            name = evdev.ecodes.KEY.get(ev.code, str(ev.code))
            name = name[0] if isinstance(name, list) else name
            if not keys or name in keys:
                booth.trigger(f"Fernbedienung {name}")


def gpio_trigger(booth, pin):
    """Taster/Buzzer zwischen GPIO-Pin und GND (Raspberry Pi)."""
    from gpiozero import Button
    button = Button(pin, bounce_time=0.1)
    button.when_pressed = lambda: booth.trigger(f"GPIO {pin}")
    print(f"GPIO-Taster an Pin {pin} aktiv")
    threading.Event().wait()


# ---------------------------------------------------------------- Webserver fuers Tablet

PAGE = """<!doctype html>
<html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Fotobox</title>
<style>
  html,body{margin:0;height:100%;background:#111;color:#fff;font-family:sans-serif;overflow:hidden}
  #main{position:absolute;inset:0 0 110px 0;display:flex;align-items:center;justify-content:center}
  #main img{max-width:100%;max-height:100%;object-fit:contain}
  #overlay{position:absolute;inset:0;display:none;align-items:center;justify-content:center;
           font-size:40vh;font-weight:bold;background:rgba(0,0,0,.6)}
  #overlay.small{font-size:8vh}
  #strip{position:absolute;left:0;right:0;bottom:0;height:110px;display:flex;gap:6px;
         overflow-x:auto;padding:5px;box-sizing:border-box;background:#000}
  #strip img{height:100px;cursor:pointer;border:2px solid transparent}
  #strip img.sel{border-color:#fff}
  #btn{position:absolute;right:20px;bottom:130px;width:130px;height:130px;border-radius:50%;
       border:0;background:#d22;color:#fff;font-size:24px;font-weight:bold}
  #msg{position:absolute;top:10px;left:10px;font-size:18px;color:#f88}
</style></head><body>
<div id="main"><span>Noch keine Fotos - Ausl&ouml;ser dr&uuml;cken!</span></div>
<div id="strip"></div>
<button id="btn">Foto!</button>
<div id="msg"></div>
<div id="overlay"></div>
<script>
const main=document.getElementById('main'), strip=document.getElementById('strip'),
      ov=document.getElementById('overlay'), msg=document.getElementById('msg');
function show(name){
  main.innerHTML='<img src="/photos/'+encodeURIComponent(name)+'">';
  for(const i of strip.children) i.classList.toggle('sel', i.dataset.name===name);
}
function addThumb(name, front){
  const i=document.createElement('img'); i.src='/photos/'+encodeURIComponent(name);
  i.dataset.name=name; i.onclick=()=>show(name);
  front ? strip.prepend(i) : strip.append(i);
}
function overlay(text, small){ ov.textContent=text; ov.className=small?'small':'';
  ov.style.display=text?'flex':'none'; }
fetch('/api/photos').then(r=>r.json()).then(list=>{
  list.forEach(n=>addThumb(n,false)); if(list.length) show(list[0]); });
document.getElementById('btn').onclick=()=>fetch('/api/trigger',{method:'POST'});
const es=new EventSource('/events');
es.onmessage=e=>{
  const ev=JSON.parse(e.data); msg.textContent='';
  if(ev.type==='countdown') overlay(ev.value);
  else if(ev.type==='capturing') overlay('Cheese!', true);
  else if(ev.type==='photo'){ overlay(''); addThumb(ev.name,true); show(ev.name); }
  else if(ev.type==='error'){ overlay(''); msg.textContent='Fehler: '+ev.message; }
};
</script></body></html>
"""


def make_handler(booth):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def _send(self, code, body, ctype):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path == "/":
                self._send(200, PAGE.encode(), "text/html; charset=utf-8")
            elif self.path == "/api/photos":
                self._send(200, json.dumps(booth.photos()).encode(), "application/json")
            elif self.path.startswith("/photos/"):
                name = os.path.basename(unquote(self.path[len("/photos/"):]))
                f = booth.web_dir / name
                if f.is_file():
                    self._send(200, f.read_bytes(), "image/jpeg")
                else:
                    self._send(404, b"not found", "text/plain")
            elif self.path == "/events":
                self._events()
            else:
                self._send(404, b"not found", "text/plain")

        def do_POST(self):
            if self.path == "/api/trigger":
                ok = booth.trigger("Tablet")
                self._send(200 if ok else 409, json.dumps({"started": ok}).encode(),
                           "application/json")
            else:
                self._send(404, b"not found", "text/plain")

        def _events(self):
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            q = booth.subscribe()
            try:
                while True:
                    try:
                        ev = q.get(timeout=15)
                        self.wfile.write(f"data: {json.dumps(ev)}\n\n".encode())
                    except queue.Empty:
                        self.wfile.write(b": keepalive\n\n")
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass
            finally:
                booth.unsubscribe(q)

    return Handler


# ---------------------------------------------------------------- Start

def main():
    ap = argparse.ArgumentParser(description="Fotobox fuer Sony a6000 mit Tablet-Anzeige")
    ap.add_argument("--photos", default="~/fotobox_bilder", help="Speicherordner")
    ap.add_argument("--port", type=int, default=8000)
    ap.add_argument("--countdown", type=int, default=3, help="Sekunden bis zur Aufnahme")
    ap.add_argument("--web-size", type=int, default=1920, help="Kantenlaenge der Tablet-Bilder")
    ap.add_argument("--evdev", metavar="GERAET", help="Fernbedienung, z.B. /dev/input/event3")
    ap.add_argument("--keys", default="", help="nur diese Tasten, z.B. KEY_VOLUMEUP,KEY_ENTER")
    ap.add_argument("--gpio", type=int, metavar="PIN", help="GPIO-Pin fuer Buzzer (BCM)")
    ap.add_argument("--simulate", action="store_true", help="ohne Kamera testen")
    args = ap.parse_args()

    photo_dir = Path(args.photos).expanduser()
    photo_dir.mkdir(parents=True, exist_ok=True)
    camera = Camera(simulate=args.simulate)
    booth = Booth(camera, photo_dir, args.countdown, args.web_size)

    threading.Thread(target=stdin_trigger, args=(booth,), daemon=True).start()
    if args.evdev:
        keys = [k.strip() for k in args.keys.split(",") if k.strip()]
        threading.Thread(target=evdev_trigger, args=(booth, args.evdev, keys), daemon=True).start()
    if args.gpio is not None:
        threading.Thread(target=gpio_trigger, args=(booth, args.gpio), daemon=True).start()

    server = ThreadingHTTPServer(("0.0.0.0", args.port), make_handler(booth))
    server.daemon_threads = True
    print(f"Fotobox laeuft: http://<IP-dieses-Rechners>:{args.port}  (Bilder: {photo_dir})")
    print("Enter druecken = ausloesen, Strg+C = beenden")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        camera.close()


if __name__ == "__main__":
    main()
