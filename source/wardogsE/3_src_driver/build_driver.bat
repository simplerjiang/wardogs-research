@echo off
setlocal
rem ============================================================
rem  CoreWD - build the kernel driver
rem  Output: driver.sys in this folder; standard non-PnP kernel service build.
rem
rem  Uses MSVC + the WDK headers directly - no WDK Visual Studio
rem  integration and no msbuild project needed.
rem  Requires: Visual Studio 2022 (MSVC x64) + Windows 11 SDK (WDK headers).
rem ============================================================
cd /d "%~dp0"

set "VS=C:\Program Files\Microsoft Visual Studio\2022\Community"
set "MSVC=14.44.35207"
set "WINSDK=C:\Program Files (x86)\Windows Kits\10"
set "SDK=10.0.26100.0"

set "CLX=%VS%\VC\Tools\MSVC\%MSVC%\bin\Hostx64\x64\cl.exe"
if not exist "%CLX%" (
    echo [ERROR] cl.exe not found: "%CLX%"
    echo         Edit MSVC / WINSDK / SDK at the top of this script.
    pause
    exit /b 1
)
rem NOTE: never name a variable LINK / CL / PATH here - link.exe reads the LINK
rem environment variable as extra command line and "C:\Program Files" splits it.
set "INC_KM=%WINSDK%\Include\%SDK%\km"
set "INC_SH=%WINSDK%\Include\%SDK%\shared"
set "INC_CRT=%WINSDK%\Include\%SDK%\km\crt"
set "LIB_KM=%WINSDK%\Lib\%SDK%\km\x64"

if not exist "%INC_KM%\ntddk.h" (
    echo [ERROR] WDK headers not found under "%INC_KM%"
    echo         Install "Windows Driver Kit" for SDK %SDK%.
    pause
    exit /b 1
)

if not exist "x64\Release" mkdir "x64\Release"

echo [1/2] compiling driver.c
"%CLX%" /c /nologo /utf-8 /W3 /O2 /D_WIN64 /D_AMD64_ /DAMD64 /DNDEBUG /MT /GS- /Zc:wchar_t /Zc:forScope /GR- ^
  /I"%INC_KM%" /I"%INC_SH%" /I"%INC_CRT%" /kernel ^
  /Fo"x64\Release\driver.obj" driver.c
if errorlevel 1 ( echo [FAILED] compile & pause & exit /b 1 )

echo [2/2] linking driver.sys
"%VS%\VC\Tools\MSVC\%MSVC%\bin\Hostx64\x64\link.exe" /OUT:"x64\Release\driver.sys" /NOLOGO ^
  /NODEFAULTLIB ^
  /LIBPATH:"%LIB_KM%" ^
  ntoskrnl.lib hal.lib wdmsec.lib BufferOverflowK.lib ^
  /SUBSYSTEM:NATIVE /DRIVER /ENTRY:DriverEntry /MACHINE:X64 /KERNEL ^
  "x64\Release\driver.obj"
if errorlevel 1 ( echo [FAILED] link & pause & exit /b 1 )

copy /Y "x64\Release\driver.sys" "driver.sys" >nul
echo.
echo BUILD_OK  -^>  x64\Release\driver.sys  (copied to driver.sys)
echo.
echo This build requires the DriverObject supplied by Windows.
echo Signing and standard service installation: see ..\STANDARD_DRIVER.md.
echo Building does not sign, install or load the driver.
exit /b 0
