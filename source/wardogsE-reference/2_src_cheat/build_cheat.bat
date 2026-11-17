@echo off
setlocal
rem ============================================================
rem  CoreWD - build the external overlay (ESP + chams + menu)
rem  Output: WarDogsESP_v2.exe in this folder.
rem  Requires: Visual Studio 2022 (Desktop C++), Windows 11 SDK.
rem  The WebView2 SDK is bundled in .\WebView2SDK - nothing to install.
rem ============================================================
cd /d "%~dp0"

set "VS=C:\Program Files\Microsoft Visual Studio\2022\Community"
if not exist "%VS%\VC\Auxiliary\Build\vcvars64.bat" (
    echo [ERROR] vcvars64.bat not found under "%VS%"
    echo         Edit the VS variable at the top of this script.
    pause
    exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo VCVARS_FAIL & exit /b 1 )

set "WV2=%~dp0WebView2SDK"
if not exist "%WV2%\include\WebView2.h" (
    echo [ERROR] WebView2 SDK missing under "%WV2%"
    pause
    exit /b 1
)

echo [1/5] wd_esp_full.c
cl /nologo /O2 /D_CRT_SECURE_NO_WARNINGS /c /EHsc /I "%WV2%\include" wd_esp_full.c /Fo:wdm_esp.obj
if errorlevel 1 goto fail

echo [2/5] chams\chams.c
cl /nologo /O2 /D_CRT_SECURE_NO_WARNINGS /c /EHsc chams\chams.c /Fo:wdm_chams.obj
if errorlevel 1 goto fail

echo [3/5] menu_webview.cpp
cl /nologo /O2 /D_CRT_SECURE_NO_WARNINGS /c /EHsc /I "%WV2%\include" menu_webview.cpp /Fo:wdm_wv2.obj
if errorlevel 1 goto fail

echo [4/5] gpu.cpp
cl /nologo /O2 /D_CRT_SECURE_NO_WARNINGS /c /EHsc gpu.cpp /Fo:wdm_gpu.obj
if errorlevel 1 goto fail

echo [5/5] link
link /nologo /SUBSYSTEM:WINDOWS /MACHINE:X64 /OUT:WarDogsESP_v2.exe ^
  wdm_esp.obj wdm_chams.obj wdm_wv2.obj wdm_gpu.obj ^
  kernel32.lib user32.lib gdi32.lib comctl32.lib advapi32.lib ole32.lib oleaut32.lib uuid.lib ^
  d3d11.lib dxgi.lib d2d1.lib dwrite.lib dcomp.lib d3dcompiler.lib ^
  /LIBPATH:"%WV2%\lib\x64" WebView2Loader.dll.lib
if errorlevel 1 goto fail

rem WebView2Loader.dll is an import-library dependency: it must sit next to the exe.
copy /Y "%WV2%\lib\x64\WebView2Loader.dll" . >nul
rem The menu is HTML loaded from disk by the exe - keep menu.html beside it.
if not exist menu.html (
    echo [WARN] menu.html is missing here - the F1 menu will stay blank.
)

echo.
echo BUILD_OK  -^>  WarDogsESP_v2.exe
echo Copy WarDogsESP_v2.exe + WebView2Loader.dll + menu.html into the run folder.
exit /b 0

:fail
echo BUILD_FAIL
pause
exit /b 1
