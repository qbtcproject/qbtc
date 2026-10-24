@echo off
if not exist "%~dp0PANEL.ps1" (
 echo Extract ALL ZIP files before opening the panel.
 pause
 exit /b 1
)
powershell.exe -NoProfile -WindowStyle Hidden -STA -ExecutionPolicy Bypass -File "%~dp0PANEL.ps1"
if errorlevel 1 pause
