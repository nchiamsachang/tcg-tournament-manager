@echo off
rem Builds, then runs both test programs.  Results are also written to build\*_results.txt.
setlocal
if "%QT_DIR%"=="" set QT_DIR=C:\Qt\6.10.2\msvc2022_64
call "%~dp0build.bat" || exit /b 1
set PATH=%QT_DIR%\bin;%PATH%
cd /d "%~dp0build"
"%~dp0build\test_core.exe" -o core_results.txt,txt
set CORE=%ERRORLEVEL%
"%~dp0build\src\ui\test_ui.exe" -o ui_results.txt,txt
set UI=%ERRORLEVEL%
findstr /b "Totals FAIL" core_results.txt ui_results.txt
if not "%CORE%%UI%"=="00" exit /b 1
