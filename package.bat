@echo off
rem Builds the app and packages it for another PC:
rem     dist\TcgTournamentManager-<version>\              the folder to run or share
rem     dist\TcgTournamentManager-<version>-win64.zip     the same folder, zipped
rem The version comes from CMakeLists.txt.  The folder holds the program, the Qt files it
rem needs, the Microsoft C++ runtime and the licence notices; it never holds a database.
setlocal
call "%~dp0vsenv.bat" || exit /b 1
call "%~dp0build.bat" || exit /b 1

set /p VERSION=<"%~dp0build\version.txt"
set "NAME=TcgTournamentManager-%VERSION%"
set "OUT=%~dp0dist\%NAME%"
set "ZIP=%~dp0dist\%NAME%-win64.zip"

if exist "%OUT%" rmdir /s /q "%OUT%"
if exist "%OUT%" echo Could not replace "%OUT%". Close the program if it is running from there.& exit /b 1
mkdir "%OUT%" || exit /b 1
copy /y "%~dp0build\src\ui\TcgTournamentManager.exe" "%OUT%\" >nul || exit /b 1

rem Qt libraries and plugins.  Left out: the touch-over-network plugin (the only thing that
rem would pull in Qt Network), the drivers for database servers the app does not use, and
rem the shader compilers.  What was copied is listed in build\windeployqt.log.
windeployqt --release --no-translations --no-system-d3d-compiler --no-system-dxc-compiler --no-opengl-sw --no-compiler-runtime --exclude-plugins qtuiotouchplugin,qsqlibase,qsqlmimer,qsqloci,qsqlodbc,qsqlpsql "%OUT%\TcgTournamentManager.exe" > "%~dp0build\windeployqt.log" || exit /b 1

rem windeployqt can copy Windows' own icuuc.dll; every supported Windows already has it.
if exist "%OUT%\icuuc.dll" fc /b "%OUT%\icuuc.dll" "%SystemRoot%\System32\icuuc.dll" >nul 2>&1 && del "%OUT%\icuuc.dll"

rem The Microsoft C++ runtime, beside the program, so nothing has to be installed first.
set "CRT="
for /d %%d in ("%VCToolsRedistDir%x64\Microsoft.VC*.CRT") do set "CRT=%%d"
if "%CRT%"=="" echo The Microsoft C++ runtime was not found under "%VCToolsRedistDir%".& exit /b 1
copy /y "%CRT%\*.dll" "%OUT%\" >nul || exit /b 1

mkdir "%OUT%\licenses" || exit /b 1
copy /y "%~dp0packaging\licenses\*" "%OUT%\licenses\" >nul || exit /b 1
powershell -NoProfile -Command "(Get-Content -Raw -LiteralPath '%~dp0packaging\README.txt').Replace('@VERSION@', '%VERSION%') | Set-Content -NoNewline -Encoding ascii -LiteralPath '%OUT%\README.txt'" || exit /b 1

rem Nothing private may be shipped.
dir /s /b "%OUT%\*.db" "%OUT%\*.db-wal" "%OUT%\*.db-shm" "%OUT%\settings.json" "%OUT%\*.log" "%OUT%\*.pdb" >nul 2>&1 && echo Refusing to package: "%OUT%" contains a database, settings, log or debug file.&& exit /b 1

if exist "%ZIP%" del "%ZIP%"
"%SystemRoot%\System32\tar.exe" -a -c -f "%ZIP%" -C "%~dp0dist" "%NAME%" || exit /b 1
echo.
echo Version: %VERSION%
echo Folder:  %OUT%
echo Zip:     %ZIP%
