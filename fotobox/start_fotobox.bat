@echo off
rem Fotobox starten (Windows). Erst --simulate ausprobieren, dann ohne.
cd /d "%~dp0"
rem Browser erst nach 2 s oeffnen, damit der Server schon laeuft
start "" /b cmd /c "timeout /t 2 >nul & start http://localhost:8000"
py fotobox.py --photos "%USERPROFILE%\Pictures\Fotobox" %*
pause
