#!/usr/bin/env bash
# Richtet beim ersten Start eine virtuelle Umgebung ein und startet das Webinterface.
cd "$(dirname "$0")"
if [ ! -d .venv ]; then
  python3 -m venv .venv
  .venv/bin/pip install -r requirements.txt
fi
.venv/bin/python app.py "$@"
