@echo off
rem Builds, then runs the three test programs.  Results are also written to *_results.txt in
rem the build folder (build, or BUILD_DIR when that is set).
setlocal
call "%~dp0vsenv.bat" || exit /b 1
call "%~dp0build.bat" || exit /b 1
cd /d "%~dp0%BUILD_DIR%"
test_core.exe -o core_results.txt,txt
set CORE=%ERRORLEVEL%
test_rules.exe -o rules_results.txt,txt
set RULES=%ERRORLEVEL%
src\ui\test_ui.exe -o ui_results.txt,txt
set UI=%ERRORLEVEL%
findstr /b "Totals FAIL XFAIL XPASS" core_results.txt rules_results.txt ui_results.txt
if not "%CORE%%RULES%%UI%"=="000" exit /b 1
