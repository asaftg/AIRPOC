@echo off
REM Double-click to stop the AIRPOC operator console service on the Jetson.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0airpoc.ps1" -Action stop
