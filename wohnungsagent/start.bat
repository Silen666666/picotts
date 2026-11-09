@echo off
REM Richtet beim ersten Start eine virtuelle Umgebung ein und startet das Webinterface.
cd /d "%~dp0"
if not exist .venv (
  py -3 -m venv .venv
  .venv\Scripts\pip install -r requirements.txt
)
.venv\Scripts\python app.py %*
