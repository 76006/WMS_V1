@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\stop-wms-listeners.ps1" -Product All
if errorlevel 1 (
    echo Failed to close all WMS listeners. See the message above.
) else (
    echo WMS servers closed. You can now start the new build.
)
pause
