@echo off
rem D3D12 overlay demo: synthetic targets, no driver, no target process.
rem Elevation is not required for this mode.
setlocal
set HERE=%~dp0
set OUT=%HERE%build
cd /d "%OUT%"

set SECONDS=%~1
if "%SECONDS%"=="" set SECONDS=8
set SCALE=%~2
if "%SCALE%"=="" set SCALE=1.0

"%OUT%\WardogLauncher.exe" --mode esp --esp-demo --esp-seconds %SECONDS% --esp-scale %SCALE% ^
  --esp-report "%OUT%\esp-demo-report.json" --esp-capture "%OUT%\esp-demo-frame.bmp" ^
  --log "%OUT%\esp-demo.jsonl" > "%OUT%\esp-demo.out.txt" 2>&1
set RC=%ERRORLEVEL%
echo EXITCODE=%RC% > "%OUT%\esp-demo-exit.txt"
type "%OUT%\esp-demo.out.txt" | findstr /C:"esp_summary" /C:"esp_overlay" /C:"exit"
exit /b %RC%
