@echo off
setlocal
if "%~1"=="" (
  echo Usage: build.cmd absolute-output-directory
  exit /b 2
)
if not defined VSCMD_VER (
  call "E:\Apps\VisualStudio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
  if errorlevel 1 exit /b 1
)
set "RECON_OUT=%~f1"
if not exist "%RECON_OUT%" (
  mkdir "%RECON_OUT%"
  if errorlevel 1 exit /b 1
)
pushd "%RECON_OUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /utf-8 /W4 /WX /EHsc /O2 /DUNICODE /D_UNICODE /DNOMINMAX "%~dp0render2d.cpp" "%~dp0render2d_test.cpp" /Fe:render2d_test.exe /link d2d1.lib dwrite.lib windowscodecs.lib ole32.lib
set "RECON_RESULT=%errorlevel%"
if not "%RECON_RESULT%"=="0" goto finish
cl /nologo /std:c++17 /utf-8 /W4 /WX /EHsc /O2 "%~dp0snapshot_test.cpp" /Fe:snapshot_test.exe
set "RECON_RESULT=%errorlevel%"
if not "%RECON_RESULT%"=="0" goto finish
cl /nologo /std:c++17 /utf-8 /W4 /WX /EHsc /O2 "%~dp0reader_paths_test.cpp" /Fe:reader_paths_test.exe
set "RECON_RESULT=%errorlevel%"
if not "%RECON_RESULT%"=="0" goto finish
cl /nologo /std:c++17 /utf-8 /W4 /WX /EHsc /O2 "%~dp0reference_rules_test.cpp" /Fe:reference_rules_test.exe
set "RECON_RESULT=%errorlevel%"
if not "%RECON_RESULT%"=="0" goto finish
cl /nologo /std:c++17 /utf-8 /W4 /WX /EHsc /O2 "%~dp0reference_pose.cpp" "%~dp0reference_pose_test.cpp" /Fe:reference_pose_test.exe
set "RECON_RESULT=%errorlevel%"
if not "%RECON_RESULT%"=="0" goto finish
cl /nologo /std:c++17 /utf-8 /W4 /WX /EHsc /O2 "%~dp0reference_projection.cpp" "%~dp0reference_projection_test.cpp" /Fe:reference_projection_test.exe
set "RECON_RESULT=%errorlevel%"
if not "%RECON_RESULT%"=="0" goto finish
cl /nologo /std:c++17 /utf-8 /W4 /WX /EHsc /O2 "%~dp0reference_pose.cpp" "%~dp0reference_mapping.cpp" "%~dp0reference_mapping_test.cpp" /Fe:reference_mapping_test.exe
set "RECON_RESULT=%errorlevel%"
if not "%RECON_RESULT%"=="0" goto finish
cl /nologo /std:c++17 /utf-8 /W4 /WX /EHsc /O2 "%~dp0reference_pose.cpp" "%~dp0reference_cache.cpp" "%~dp0reference_cache_test.cpp" /Fe:reference_cache_test.exe
set "RECON_RESULT=%errorlevel%"
if not "%RECON_RESULT%"=="0" goto finish
cl /nologo /std:c++17 /utf-8 /W4 /WX /EHsc /O2 "%~dp0reference_pose.cpp" "%~dp0reference_cache.cpp" "%~dp0reference_consistency.cpp" "%~dp0reference_consistency_test.cpp" /Fe:reference_consistency_test.exe
set "RECON_RESULT=%errorlevel%"
if not "%RECON_RESULT%"=="0" goto finish
cl /nologo /std:c++17 /utf-8 /W4 /WX /EHsc /O2 "%~dp0menu_config.cpp" "%~dp0menu_config_test.cpp" /Fe:menu_config_test.exe
set "RECON_RESULT=%errorlevel%"
if not "%RECON_RESULT%"=="0" goto finish
cl /nologo /std:c++17 /utf-8 /W4 /WX /EHsc /O2 /DUNICODE /D_UNICODE /DNOMINMAX "%~dp0render2d.cpp" "%~dp0menu_config.cpp" "%~dp0preview_scene.cpp" "%~dp0preview_scene_test.cpp" /Fe:preview_scene_test.exe /link d2d1.lib dwrite.lib windowscodecs.lib ole32.lib
set "RECON_RESULT=%errorlevel%"
if not "%RECON_RESULT%"=="0" goto finish
cl /nologo /std:c++17 /utf-8 /W4 /WX /EHsc /O2 /DUNICODE /D_UNICODE /DNOMINMAX "%~dp0render2d.cpp" "%~dp0menu_config.cpp" "%~dp0preview_scene.cpp" "%~dp0preview.cpp" /Fe:preview.exe /link d2d1.lib dwrite.lib windowscodecs.lib ole32.lib user32.lib gdi32.lib
set "RECON_RESULT=%errorlevel%"
if not "%RECON_RESULT%"=="0" goto finish
cl /nologo /std:c++17 /utf-8 /W4 /WX /EHsc /O2 /DUNICODE /D_UNICODE /DNOMINMAX "%~dp0render2d.cpp" "%~dp0reference_pose.cpp" "%~dp0reference_projection.cpp" "%~dp0reference_mapping.cpp" "%~dp0reference_cache.cpp" "%~dp0reference_consistency.cpp" "%~dp0controlled_scene.cpp" "%~dp0controlled_scene_test.cpp" /Fe:controlled_scene_test.exe /link d2d1.lib dwrite.lib windowscodecs.lib ole32.lib
set "RECON_RESULT=%errorlevel%"
if not "%RECON_RESULT%"=="0" goto finish
cl /nologo /std:c++17 /utf-8 /W4 /WX /EHsc /O2 /DUNICODE /D_UNICODE /DNOMINMAX "%~dp0render2d.cpp" "%~dp0reference_pose.cpp" "%~dp0reference_projection.cpp" "%~dp0reference_mapping.cpp" "%~dp0reference_cache.cpp" "%~dp0reference_consistency.cpp" "%~dp0controlled_scene.cpp" "%~dp0overlay.cpp" /Fe:overlay.exe /link d2d1.lib dwrite.lib windowscodecs.lib ole32.lib user32.lib gdi32.lib dwmapi.lib
set "RECON_RESULT=%errorlevel%"
:finish
popd
exit /b %RECON_RESULT%
