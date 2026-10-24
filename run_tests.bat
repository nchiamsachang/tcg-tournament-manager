@echo off
rem builds, then runs the three test programs and prints their totals.  results are written
rem to *_results.txt in the build folder (build, or BUILD_DIR when that is set).  each run
rem deletes the old result files first, so only this run's results are ever shown.  exits
rem with 1 when the build fails, a test fails, or a test program cannot be started.
setlocal
call "%~dp0vsenv.bat" || exit /b 1
call "%~dp0build.bat" || exit /b 1
set "OUT=%~dp0%BUILD_DIR%"
set "BUILD_ID=unknown"
if exist "%OUT%\build_id.txt" set /p BUILD_ID=<"%OUT%\build_id.txt"
echo Testing build %BUILD_ID% in "%OUT%"
set FAILED=0
call :run test_core "%OUT%\test_core.exe" "%OUT%\core_results.txt"
call :run test_rules "%OUT%\test_rules.exe" "%OUT%\rules_results.txt"
call :run test_ui "%OUT%\src\ui\test_ui.exe" "%OUT%\ui_results.txt"
if not "%FAILED%"=="0" (
    echo FAILED: at least one test program failed or did not run.
    exit /b 1
)
echo All three test programs passed.
exit /b 0

rem :run name program results
rem runs one test program by its full path and reports only the result file it wrote just now
:run
if exist "%~3" del /f /q "%~3"
if exist "%~3" (
    echo %~1: the old result file "%~3" could not be removed.
    set FAILED=1
    exit /b 0
)
if not exist "%~2" (
    echo %~1: "%~2" does not exist.
    set FAILED=1
    exit /b 0
)
for /f "skip=1 delims=" %%h in ('certutil -hashfile "%~2" SHA256') do (
    echo %~1: "%~2" sha256 %%h
    goto :hashed
)
:hashed
"%~2" -o "%~3",txt
set CODE=%ERRORLEVEL%
if not exist "%~3" (
    echo %~1: wrote no results ^(exit code %CODE%^); it may not have started.
    set FAILED=1
    exit /b 0
)
for /f "delims=" %%l in ('findstr /b "Totals FAIL XFAIL XPASS" "%~3"') do echo %~1: %%l
if not "%CODE%"=="0" (
    echo %~1: exit code %CODE%.
    set FAILED=1
)
exit /b 0
