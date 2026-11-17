@echo off
rem Elevated, read-only case run: no --allow-write anywhere in this file.
cd /d "%~dp0build"

rem An earlier crash can leave the case fixture (and the helper service) behind.
taskkill /f /im WardogFixture.exe >nul 2>&1
del /q fixture-status.json >nul 2>&1

WardogLauncher.exe --mode cleanup --log cleanup.jsonl > cleanup.out.txt 2>&1

WardogLauncher.exe --mode case --spawn-target --log case-run.jsonl > case-run.out.txt 2>&1
set RC=%ERRORLEVEL%
echo EXITCODE=%RC% > case-exit.txt
taskkill /f /im WardogFixture.exe >nul 2>&1
exit /b %RC%
