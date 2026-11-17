@echo off
rem Elevated, read-only: read a live process at an explicit image base.
rem No --allow-write anywhere in this file.
setlocal
set OUT=%~dp0build
set TARGET=%~1
if "%TARGET%"=="" set TARGET=WardogsClient-Win64-Shipping.exe
set BASE=%~2
if "%BASE%"=="" set BASE=0x7FF653000000

"%OUT%\WardogLauncher.exe" --mode cleanup --log "%OUT%\cleanup.jsonl" > "%OUT%\cleanup.out.txt" 2>&1
"%OUT%\WardogLauncher.exe" --mode read --process "%TARGET%" --base %BASE% --len 256 --log "%OUT%\game-read-base.jsonl" > "%OUT%\game-read-base.out.txt" 2>&1
set RC=%ERRORLEVEL%
echo EXITCODE=%RC% > "%OUT%\game-read-base-exit.txt"
exit /b %RC%
