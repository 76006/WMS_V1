@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\start-lan-client.ps1" -Product IceBeauty -StartLocalServer
if errorlevel 1 pause
