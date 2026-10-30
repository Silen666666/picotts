@echo off
rem Startet das Installationsskript mit Administratorrechten.
cd /d "%~dp0"
powershell -NoProfile -Command "Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile -ExecutionPolicy Bypass -NoExit -File \"%~dp0install_fotobox.ps1\"'"
