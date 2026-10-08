@echo off
rem Builds the C++ app and its tests (Release, 64-bit) into build.  Needs Visual Studio 2022
rem (C++ workload) and Qt 6 for MSVC; set QT_DIR if Qt is not in C:\Qt\6.10.2\msvc2022_64.
setlocal
call "%~dp0vsenv.bat" || exit /b 1
cd /d "%~dp0"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%QT_DIR%" || exit /b 1
cmake --build build %* || exit /b 1
