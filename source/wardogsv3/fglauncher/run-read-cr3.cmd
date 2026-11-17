@echo off
rem Elevated, read-only: read a live process through a supplied page-table root.
rem No --allow-write anywhere in this file.
setlocal
set OUT=%~dp0build
set TARGET=%~1
if "%TARGET%"=="" set TARGET=WardogsClient-Win64-Shipping.exe
set BASE=%~2
if "%BASE%"=="" set BASE=0x7FF653000000
set CR3=%~3
if "%CR3%"=="" set CR3=0x29883000

"%OUT%\WardogLauncher.exe" --mode cleanup --log "%OUT%\cleanup.jsonl" > "%OUT%\cleanup.out.txt" 2>&1
"%OUT%\WardogLauncher.exe" --mode read --process "%TARGET%" --base %BASE% --cr3 %CR3% --len 256 --log "%OUT%\game-read-cr3.jsonl" > "%OUT%\game-read-cr3.out.txt" 2>&1
set RC=%ERRORLEVEL%
echo EXITCODE=%RC% > "%OUT%\game-read-cr3-exit.txt"
exit /b %RC%
