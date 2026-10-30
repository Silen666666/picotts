# Installiert alles fuer die Fotobox unter Windows:
#   Python, MSYS2, gphoto2 (MSYS2-Paket), Pillow, und oeffnet Zadig fuer den USB-Treiber.
# Start ueber install_fotobox.bat (fragt nach Administratorrechten).

$ErrorActionPreference = "Stop"
$msys = "C:\msys64"
$bash = "$msys\usr\bin\bash.exe"
$gphoto2 = "$msys\ucrt64\bin\gphoto2.exe"

function Step($text) { Write-Host "`n=== $text ===" -ForegroundColor Cyan }

if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    Write-Host "winget fehlt. Bitte 'App-Installer' aus dem Microsoft Store installieren und erneut starten." -ForegroundColor Red
    exit 1
}

Step "1/5 Python"
if (Get-Command py -ErrorAction SilentlyContinue) {
    Write-Host "Python ist bereits installiert."
} else {
    winget install --id Python.Python.3.12 -e --accept-package-agreements --accept-source-agreements
    $env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" +
                [Environment]::GetEnvironmentVariable("Path", "User")
}

Step "2/5 MSYS2"
if (Test-Path $bash) {
    Write-Host "MSYS2 ist bereits installiert."
} else {
    winget install --id MSYS2.MSYS2 -e --accept-package-agreements --accept-source-agreements
    if (-not (Test-Path $bash)) {
        Write-Host "MSYS2 wurde nicht unter $msys gefunden. Bitte manuell von https://www.msys2.org installieren." -ForegroundColor Red
        exit 1
    }
}

Step "3/5 gphoto2 (MSYS2-Paket)"
# Das erste Update aktualisiert evtl. nur das Grundsystem, daher zweimal.
& $bash -lc "pacman -Syu --noconfirm"
& $bash -lc "pacman -Syu --noconfirm"
& $bash -lc "pacman -S --needed --noconfirm mingw-w64-ucrt-x86_64-gphoto2"
if (-not (Test-Path $gphoto2)) {
    Write-Host "gphoto2.exe fehlt unter $gphoto2 - Installation fehlgeschlagen." -ForegroundColor Red
    exit 1
}
& $gphoto2 --version | Select-Object -First 1

Step "4/5 Pillow (verkleinerte Bilder fuers Tablet)"
py -m pip install --upgrade pillow

Step "5/5 USB-Treiber (Zadig)"
Write-Host @"
Jetzt den USB-Treiber tauschen (nur einmal noetig):
  1. Kamera: MENU > Einstellungen > USB-Verbindung = PC-Fernbedienung
  2. Kamera einschalten und per USB anstecken (Datenkabel, kein reines Ladekabel!)
  3. Zadig starten (Webseite oeffnet sich gleich), Options > List All Devices
  4. Sony-Kamera (ILCE-6000) auswaehlen, Ziel-Treiber 'WinUSB', 'Replace Driver'
"@
Start-Process "https://zadig.akeo.ie/"
Read-Host "Wenn Zadig fertig ist: Enter druecken, um die Kamera zu testen"

Step "Test"
& $gphoto2 --auto-detect
Write-Host "`nErscheint oben 'Sony Alpha-A6000 (Control)', ist alles bereit: start_fotobox.bat starten." -ForegroundColor Green
