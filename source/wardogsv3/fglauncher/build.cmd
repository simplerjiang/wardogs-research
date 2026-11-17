@echo off
setlocal enabledelayedexpansion
set ROOT=%~dp0
call "E:\Apps\VisualStudio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
  echo [build] vcvars64 failed
  exit /b 1
)

if not exist "%ROOT%build" mkdir "%ROOT%build"
if not exist "%ROOT%build\obj" mkdir "%ROOT%build\obj"
pushd "%ROOT%build"

> ntoskrnl.def echo LIBRARY ntoskrnl.exe
>> ntoskrnl.def echo EXPORTS
>> ntoskrnl.def echo IoCreateDevice
>> ntoskrnl.def echo IoCreateSymbolicLink
>> ntoskrnl.def echo IoDeleteSymbolicLink
>> ntoskrnl.def echo IoDeleteDevice

lib /nologo /def:ntoskrnl.def /machine:x64 /out:ntoskrnl.lib
if errorlevel 1 goto :fail

cl /nologo /utf-8 /c /O1 /kernel /GS- /Zl /Fo"obj\WardogProbe.obj" "%ROOT%driver\WardogProbe.c"
if errorlevel 1 goto :fail

link /nologo /driver /subsystem:native /entry:DriverEntry /nodefaultlib /machine:x64 /incremental:no ^
     /out:WardogProbe.sys "obj\WardogProbe.obj" ntoskrnl.lib
if errorlevel 1 goto :fail

cl /nologo /utf-8 /std:c++20 /EHsc /W4 /O2 /Fo"obj\\" ^
   "%ROOT%src\main.cpp" "%ROOT%src\service.cpp" "%ROOT%src\pgr.cpp" ^
   "%ROOT%src\translate.cpp" "%ROOT%src\ci.cpp" "%ROOT%src\selftest.cpp" ^
   "%ROOT%src\case.cpp" "%ROOT%src\target.cpp" ^
   "%ROOT%src\esp_projection.cpp" "%ROOT%src\esp_scene.cpp" "%ROOT%src\esp_live.cpp" ^
   "%ROOT%src\overlay_d3d12.cpp" ^
   /Fe:WardogLauncher.exe ^
   /link advapi32.lib bcrypt.lib psapi.lib d3d12.lib dxgi.lib d3d11.lib d3dcompiler.lib dcomp.lib dwmapi.lib user32.lib gdi32.lib /MAP:"WardogLauncher.map"
if errorlevel 1 goto :fail

cl /nologo /utf-8 /std:c++20 /EHsc /W4 /O2 /Fo"obj\fixture.obj" ^
   "%ROOT%src\fixture.cpp"
if errorlevel 1 goto :fail

link /nologo /subsystem:console /machine:x64 /incremental:no ^
     /BASE:0x180000000 /DYNAMICBASE:NO /FIXED ^
     /out:WardogFixture.exe "obj\fixture.obj" advapi32.lib bcrypt.lib
if errorlevel 1 goto :fail

popd
echo.
echo [build] ok:
echo   %ROOT%build\WardogLauncher.exe
echo   %ROOT%build\WardogProbe.sys
echo   %ROOT%build\WardogFixture.exe
exit /b 0

:fail
popd
echo [build] failed
exit /b 1
