@echo off
rem Elevated, read-only: pass-through runner. No --allow-write anywhere.
setlocal
set OUT=%~dp0build
"%OUT%\WardogLauncher.exe" --mode cleanup --log "%OUT%\cleanup.jsonl" > "%OUT%\cleanup.out.txt" 2>&1
"%OUT%\WardogLauncher.exe" %* > "%OUT%\probe.out.txt" 2>&1
set RC=%ERRORLEVEL%
echo EXITCODE=%RC% > "%OUT%\probe-exit.txt"
exit /b %RC%
