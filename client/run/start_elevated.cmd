@echo off
cd /d "%~dp0"
rem Фоновое приложение (GUI-подсистема): окна консоли нет, логи пишет само
rem в foxmon.log рядом с exe.
start "" "%~dp0FoxMonitor.exe"