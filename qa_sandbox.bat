@echo off
rem Starts the built app on a disposable data folder, for manual testing.  Your own players and
rem tournaments (%LOCALAPPDATA%\TcgTournamentManager) are not opened.  Run build.bat first.
rem   qa_sandbox.bat          keep what earlier test sessions saved
rem   qa_sandbox.bat fresh    start from an empty database
setlocal
if "%QT_DIR%"=="" set "QT_DIR=C:\Qt\6.10.2\msvc2022_64"
set "TCG_DATA_DIR=%TEMP%\tcg-qa-sandbox"
if /i "%~1"=="fresh" if exist "%TCG_DATA_DIR%" rmdir /s /q "%TCG_DATA_DIR%"
if not exist "%~dp0build\src\ui\TcgTournamentManager.exe" (
    echo The app has not been built yet. Run build.bat first.
    exit /b 1
)
set "PATH=%QT_DIR%\bin;%PATH%"
echo Test data folder: %TCG_DATA_DIR%
start "" "%~dp0build\src\ui\TcgTournamentManager.exe"
