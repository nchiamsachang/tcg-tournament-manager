@echo off
rem Builds the app and gathers it with the Qt files it needs into dist\TcgTournamentManager,
rem a folder that runs on a PC without Qt or Visual Studio installed.
setlocal
if "%QT_DIR%"=="" set QT_DIR=C:\Qt\6.10.2\msvc2022_64
call "%~dp0build.bat" || exit /b 1
set OUT=%~dp0dist\TcgTournamentManager
if not exist "%OUT%" mkdir "%OUT%"
copy /y "%~dp0build\src\ui\TcgTournamentManager.exe" "%OUT%\" >nul || exit /b 1
"%QT_DIR%\bin\windeployqt.exe" --release --no-translations --no-system-d3d-compiler --no-opengl-sw --compiler-runtime "%OUT%\TcgTournamentManager.exe" >nul || exit /b 1
echo Ready: %OUT%\TcgTournamentManager.exe
