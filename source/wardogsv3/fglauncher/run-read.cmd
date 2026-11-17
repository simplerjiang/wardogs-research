@echo off
rem Elevated, read-only: read a live process through the physical map.
rem No --allow-write anywhere in this file.
setlocal
set HERE=%~dp0
set OUT=%HERE%build
echo START %DATE% %TIME% > "%OUT%\read-started.txt"
cd /d "%OUT%" 2>>"%OUT%\read-started.txt"
if errorlevel 1 echo CD_FAILED >> "%OUT%\read-started.txt"
echo CWD=%CD% >> "%OUT%\read-started.txt"

set TARGET=%~1
if "%TARGET%"=="" set TARGET=WardogsClient-Win64-Shipping.exe

"%OUT%\WardogLauncher.exe" --mode cleanup --log "%OUT%\cleanup.jsonl" > "%OUT%\cleanup.out.txt" 2>&1
echo CLEANUP_RC=%ERRORLEVEL% >> "%OUT%\read-started.txt"

"%OUT%\WardogLauncher.exe" --mode read --process "%TARGET%" --len 256 --log "%OUT%\game-read.jsonl" > "%OUT%\game-read.out.txt" 2>&1
set RC=%ERRORLEVEL%
echo EXITCODE=%RC% > "%OUT%\game-read-exit.txt"
echo READ_RC=%RC% >> "%OUT%\read-started.txt"
exit /b %RC%
