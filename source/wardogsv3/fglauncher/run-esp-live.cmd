@echo off
rem Elevated: overlay anchored on the live game window, camera and actors read
rem through the physical map with the offset profile. No --allow-write anywhere.
setlocal
set HERE=%~dp0
set OUT=%HERE%build
cd /d "%OUT%"

set SECONDS=%~1
if "%SECONDS%"=="" set SECONDS=8
set PROFILE=%HERE%offsets\wardog-esp.json
if not exist "%PROFILE%" set PROFILE=

"%OUT%\WardogLauncher.exe" --mode cleanup --log "%OUT%\esp-cleanup.jsonl" > "%OUT%\esp-cleanup.out.txt" 2>&1

if "%PROFILE%"=="" (
  "%OUT%\WardogLauncher.exe" --mode esp --esp-live --process WardogsClient-Win64-Shipping.exe ^
    --esp-seconds %SECONDS% --esp-report "%OUT%\esp-live-report.json" ^
    --log "%OUT%\esp-live.jsonl" > "%OUT%\esp-live.out.txt" 2>&1
) else (
  "%OUT%\WardogLauncher.exe" --mode esp --esp-live --process WardogsClient-Win64-Shipping.exe ^
    --esp-offsets "%PROFILE%" --esp-seconds %SECONDS% --esp-report "%OUT%\esp-live-report.json" ^
    --log "%OUT%\esp-live.jsonl" > "%OUT%\esp-live.out.txt" 2>&1
)
set RC=%ERRORLEVEL%
echo EXITCODE=%RC% > "%OUT%\esp-live-exit.txt"
type "%OUT%\esp-live.out.txt" | findstr /C:"esp_" /C:"exit" /C:"error"
exit /b %RC%
