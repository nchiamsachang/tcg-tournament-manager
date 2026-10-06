@echo off
rem Builds the C++ app and its tests into build.  Needs Visual Studio 2022
rem (C++ workload) and Qt 6 for MSVC; set QT_DIR if Qt is not in C:\Qt\6.10.2.
setlocal
if "%QT_DIR%"=="" set QT_DIR=C:\Qt\6.10.2\msvc2022_64
set VS=C:\Program Files\Microsoft Visual Studio\2022\Community
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%QT_DIR%\bin;%PATH%
cd /d "%~dp0"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%QT_DIR%" || exit /b 1
cmake --build build %* || exit /b 1
