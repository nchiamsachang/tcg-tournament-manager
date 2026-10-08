@echo off
rem Puts the MSVC compiler, CMake, Ninja and Qt on PATH for the script that calls this.
rem Qt: set QT_DIR to the Qt kit folder (the one that contains bin\windeployqt.exe).
rem Visual Studio 2022 with the C++ workload is found through its installer.
if "%QT_DIR%"=="" set "QT_DIR=C:\Qt\6.10.2\msvc2022_64"
if exist "%QT_DIR%\bin\windeployqt.exe" goto :qt_ok
echo Qt was not found in "%QT_DIR%". Set QT_DIR to your Qt 6 MSVC 64-bit folder.
exit /b 1
:qt_ok
if defined VCToolsRedistDir goto :vs_ok
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS="
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not "%VS%"=="" goto :vs_found
echo Visual Studio with the "Desktop development with C++" workload was not found.
exit /b 1
:vs_found
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:vs_ok
set "PATH=%VSINSTALLDIR%Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSINSTALLDIR%Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%QT_DIR%\bin;%PATH%"
exit /b 0
