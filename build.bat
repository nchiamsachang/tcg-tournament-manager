@echo off
rem Builds the C++ app and its tests (Release, 64-bit) into build.  Needs Visual Studio 2022
rem (C++ workload) and Qt 6 for MSVC; set QT_DIR if Qt is not in C:\Qt\6.11.3\msvc2022_64.
rem Set BUILD_DIR to build somewhere other than build.
setlocal
call "%~dp0vsenv.bat" || exit /b 1
cd /d "%~dp0"
rem a folder configured for another qt kit is configured again from scratch, so a build never mixes two
set "FRESH="
if exist "%BUILD_DIR%\CMakeCache.txt" (findstr /i /l /c:"%QT_DIR:\=/%/lib/cmake/Qt6" "%BUILD_DIR%\CMakeCache.txt" >nul || set "FRESH=--fresh")
if defined FRESH echo %BUILD_DIR% was configured for another Qt kit; configuring it again for %QT_DIR%
cmake %FRESH% -S . -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%QT_DIR%" || exit /b 1
cmake --build "%BUILD_DIR%" %* || exit /b 1
