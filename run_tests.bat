@echo off
rem builds, then runs the test programs and prints their totals: all three suites with no
rem arguments, or one suite or some of its functions (run_tests help).  results are written
rem to *_results.txt in the build folder (build, or BUILD_DIR when that is set).  all three
rem old result files are deleted before building, so only this run's results are ever shown.
rem exits with 1 when the build fails, a test fails, a requested function did not run or was
rem only skipped, or a test program cannot be started, and with 2 for invalid arguments.
setlocal
set "SUITE="
set "FUNCS="
if [%1]==[] goto :args_done
if /i "%~1"=="help" goto :help
if /i "%~1"=="-h" goto :help
if /i "%~1"=="--help" goto :help
if "%~1"=="/?" goto :help
for %%s in (core rules ui) do if /i "%~1"=="%%s" set "SUITE=%%s"
if not defined SUITE goto :bad_suite
shift
:next_function
if [%1]==[] goto :args_done
rem only plain names, so nothing reaches the test program as an option such as -o
set "TCG_RT_NAME=%~1"
set TCG_RT_NAME | findstr /r /x "TCG_RT_NAME=[abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_][abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_]*" >nul || goto :bad_function
set "FUNCS=%FUNCS% %~1"
shift
goto :next_function

:args_done
set "TCG_RT_NAME="
call "%~dp0vsenv.bat" || exit /b 1
set "OUT=%~dp0%BUILD_DIR%"
for %%r in (core rules ui) do call :clear "%OUT%\%%r_results.txt" || exit /b 1
rem a selected suite builds only its own program and what it links, so it never runs an older binary
if defined SUITE goto :build_one
call "%~dp0build.bat" || exit /b 1
goto :built
:build_one
call "%~dp0build.bat" --target test_%SUITE% || exit /b 1
:built
set "BUILD_ID=unknown"
if exist "%OUT%\build_id.txt" set /p BUILD_ID=<"%OUT%\build_id.txt"
echo Testing build %BUILD_ID% in "%OUT%"
set FAILED=0
if defined SUITE goto :targeted
echo Requested: all three suites
call :run test_core "%OUT%\test_core.exe" "%OUT%\core_results.txt"
call :run test_rules "%OUT%\test_rules.exe" "%OUT%\rules_results.txt"
call :run test_ui "%OUT%\src\ui\test_ui.exe" "%OUT%\ui_results.txt"
if not "%FAILED%"=="0" (
    echo FAILED: at least one test program failed or did not run.
    exit /b 1
)
echo All three test programs passed.
exit /b 0

:targeted
set "EXE=%OUT%\test_%SUITE%.exe"
if "%SUITE%"=="ui" set "EXE=%OUT%\src\ui\test_ui.exe"
if "%SUITE%"=="core" set "NOT_RUN=rules ui"
if "%SUITE%"=="rules" set "NOT_RUN=core ui"
if "%SUITE%"=="ui" set "NOT_RUN=core rules"
if defined FUNCS (echo Requested: suite %SUITE%, functions%FUNCS%) else echo Requested: suite %SUITE%, all of its functions
call :run test_%SUITE% "%EXE%" "%OUT%\%SUITE%_results.txt"
if defined FUNCS for %%f in (%FUNCS%) do call :ran test_%SUITE% "%OUT%\%SUITE%_results.txt" %%f
if not "%FAILED%"=="0" (
    echo FAILED: targeted run of suite %SUITE%. Not run: %NOT_RUN%.
    exit /b 1
)
if defined FUNCS (echo Targeted run passed: suite %SUITE%, functions%FUNCS%.) else echo Targeted run passed: suite %SUITE%.
echo This was not a full run. Not run: %NOT_RUN%.
exit /b 0

:help
echo Usage: run_tests [suite [function ...]]
echo   run_tests                  build everything and run all three suites
echo   run_tests core             build and run one suite: core, rules or ui
echo   run_tests rules fnA fnB    run only those test functions of the rules suite
echo   run_tests help             show this text
echo Function names are as test_rules.exe -functions lists them, without the ().
echo The test programs need TCG_DATA_DIR set to a disposable folder.
echo Exit codes: 0 passed, 1 build or test failure, 2 invalid arguments.
exit /b 0

:bad_suite
echo run_tests: unknown suite. Use core, rules or ui, or see run_tests help.
exit /b 2

:bad_function
echo run_tests: invalid test function name. Use plain names such as fnA, or see run_tests help.
exit /b 2

rem :clear file
rem fails when an old result file cannot be deleted, so old totals are never shown as new
:clear
if exist "%~1" del /f /q "%~1"
if not exist "%~1" exit /b 0
echo the old result file "%~1" could not be removed.
exit /b 1

rem :run name program results
rem runs one test program by its full path and reports only the result file it wrote just now
:run
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
"%~2" -o "%~3",txt%FUNCS%
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

rem :ran name results function
rem a requested function counts only when the results hold its own outcome line (pass, fail,
rem expected or blacklisted), not just a skip
:ran
findstr /r /c:"Function not found: %~3$" "%~2" >nul && goto :ran_unknown
set "SEEN="
for /f "delims=" %%l in ('findstr /r /c:"^[BX]*PASS  *: [A-Za-z0-9_:]*::%~3(" /c:"^[BX]*FAIL!*  *: [A-Za-z0-9_:]*::%~3(" /c:"^SKIP  *: [A-Za-z0-9_:]*::%~3(" "%~2"') do (
    echo %~1 ran: %%l
    set "SEEN=1"
)
if not defined SEEN goto :ran_missing
findstr /r /c:"^[BX]*PASS  *: [A-Za-z0-9_:]*::%~3(" /c:"^[BX]*FAIL!*  *: [A-Za-z0-9_:]*::%~3(" "%~2" >nul || goto :ran_skipped
exit /b 0
:ran_skipped
echo %~1: %~3 was only skipped, so it checked nothing.
set FAILED=1
exit /b 0
:ran_unknown
echo %~1: %~3 is not a test function of this suite.
set FAILED=1
exit /b 0
:ran_missing
echo %~1: %~3 did not run.
set FAILED=1
exit /b 0
