@echo off
REM Double-click to start the AIRPOC operator console and open it in the browser.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0airpoc.ps1" -Action start
