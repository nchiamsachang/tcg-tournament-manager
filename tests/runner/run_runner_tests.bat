@echo off
rem checks run_tests.bat itself against stub build scripts and a fake test program.  run it
rem after changing run_tests.bat; it needs Python 3 (the py launcher) and builds nothing else.
rem exits with 0 when every check passes.
setlocal
call "%~dp0..\..\vsenv.bat" || exit /b 1
rem a scratch folder with spaces, so the checks also cover paths with spaces
set "WORK=%TEMP%\tcg runner check %RANDOM%%RANDOM%"
mkdir "%WORK%" || exit /b 1
cl /nologo /O1 /W3 "%~dp0fake_qtest.cpp" /Fo"%WORK%\fake_qtest.obj" /Fe"%WORK%\fake_qtest.exe" >"%WORK%\cl.log" || goto :cl_failed
py -3 -I "%~dp0test_run_tests.py" "%WORK%\fake_qtest.exe" "%WORK%"
set CODE=%ERRORLEVEL%
rmdir /s /q "%WORK%"
exit /b %CODE%

:cl_failed
type "%WORK%\cl.log"
rmdir /s /q "%WORK%"
exit /b 1
