@echo off
rem SkinnerBox++ build — locates MSVC via vswhere, emits build\SkinnerBoxPP.dll (x64).
setlocal enabledelayedexpansion
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo vswhere.exe not found - install Visual Studio Build Tools with the C++ workload.
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
    echo No MSVC C++ toolchain found.
    exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

if not exist build mkdir build
cl /nologo /W4 /EHsc /std:c++17 /O2 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX ^
   /Fobuild\ /Febuild\SkinnerBoxPP.dll /LD ^
   src\core\estimator.cpp src\core\policy.cpp src\core\content.cpp ^
   src\adapters\log_adapter.cpp src\adapters\raw_log.cpp ^
   src\adapters\intiface_adapter.cpp src\adapters\sfx.cpp ^
   src\plugin\npp_visual_adapter.cpp src\plugin\coin_overlay.cpp ^
   src\plugin\plugin_main.cpp ^
   user32.lib shell32.lib winhttp.lib winmm.lib gdi32.lib
if errorlevel 1 exit /b 1
echo.
echo Built build\SkinnerBoxPP.dll

rem Offline replay harness (core only, no Notepad++ dependencies).
cl /nologo /W4 /EHsc /std:c++17 /O2 /utf-8 /DNOMINMAX ^
   /Fobuild\ /Febuild\replay.exe ^
   test\replay.cpp src\core\estimator.cpp src\core\policy.cpp src\core\content.cpp
if errorlevel 1 exit /b 1
echo Built build\replay.exe

rem Interactive console demo (core + Intiface, no Notepad++ dependencies).
cl /nologo /W4 /EHsc /std:c++17 /O2 /utf-8 /DNOMINMAX ^
   /Fobuild\ /Febuild\demo.exe ^
   test\demo.cpp src\core\estimator.cpp src\core\policy.cpp src\core\content.cpp ^
   src\adapters\intiface_adapter.cpp ^
   winhttp.lib
if errorlevel 1 exit /b 1
echo Built build\demo.exe

rem Headless core unit tests (no WinHTTP / Notepad++).
cl /nologo /W4 /EHsc /std:c++17 /O2 /utf-8 /DNOMINMAX ^
   /Fobuild\ /Febuild\unit.exe ^
   test\unit.cpp src\core\estimator.cpp src\core\policy.cpp src\core\content.cpp
if errorlevel 1 exit /b 1
echo Built build\unit.exe
build\unit.exe
if errorlevel 1 exit /b 1
