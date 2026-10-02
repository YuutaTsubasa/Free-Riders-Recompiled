@echo off
rem Runs the benchmark on this PC and leaves a copy that is safe to send to others.
rem   run_benchmark.bat speed        baseline against the polling it replaced for suspended guests, 6 rounds
rem   run_benchmark.bat pipelines    a new player first race, with and without the prepared pipelines
rem   run_benchmark.bat exe          two builds, sfr_cpu_diagnostic_a.exe against _b.exe, in the same rounds
rem   run_benchmark.bat ctrl         a control: the same build twice, to see what the method says about no difference
rem   run_benchmark.bat speed 8      the same with 8 rounds
rem   run_benchmark.bat author       the original's own unconfirmed switches: constant upload reuse, priority, host timing
rem   run_benchmark.bat speed 2 nostretch   the menu words by presents alone (cannot finish on a fast PC)
setlocal
cd /d "%~dp0"
rem An empty src folder lets an older benchmark.ps1 skip its check of the sources
if not exist "src" mkdir "src"
set MODE=%1
if "%MODE%"=="" set MODE=speed
set REPEATS=%2
set CONFIGS=baseline,no-suspend-notify
set EXTRA=
if "%REPEATS%"=="" set REPEATS=6
if /i "%MODE%"=="author" set CONFIGS=baseline,constant-reuse,priority,no-host-timing
if /i "%MODE%"=="exe" set CONFIGS=exe-a,exe-b
if /i "%MODE%"=="ctrl" set CONFIGS=exe-b,exe-b-again
if /i "%MODE%"=="pipelines" (
    set CONFIGS=baseline,no-prewarm
    set EXTRA=-ColdPipelines
    if "%2"=="" set REPEATS=4
)
if /i "%3"=="nostretch" set EXTRA=%EXTRA% -NoStretch
if not exist "out\build\host\sfr_cpu_diagnostic.exe" goto noexe
set GAMEARGS=
rem The DXC the renderer links pixel shaders with, when the kit carries one
if exist "%~dp0dxc\dxcompiler.dll" set SFR_DXC_LIBRARY=%~dp0dxc
if exist "out\recomp\image-loader" if exist "private\assets" goto run
if exist "out\build\host\settings.ini" goto run
echo The game folders are not inside this folder.
set /p IMAGE=Folder with the game image (image-loader):
set /p ASSETS=Folder with the game assets:
set GAMEARGS=-ImageDirectory "%IMAGE%" -AssetDirectory "%ASSETS%"
:run
echo.
echo The benchmark plays the game by itself. Do not use this PC and close other programs
echo until it says it is done. Mode %MODE%, %REPEATS% rounds.
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "scripts\benchmark.ps1" -SkipBuildCheck -Configs %CONFIGS% -Repeats %REPEATS% %EXTRA% %GAMEARGS%
echo.
set LATEST=
for /f "delims=" %%Z in ('dir /b /o-d "out\bench\*-shareable*.zip" 2^>nul') do (
    set LATEST=%%Z
    goto found
)
echo No -shareable.zip was made (Python is probably missing).
echo Send the newest folder in out\bench instead, and it will be prepared on the other PC.
goto done
:found
echo Send this file: out\bench\%LATEST%
if not "%LATEST:-part=%"=="%LATEST%" echo It is one of several parts (each under 29 MB): send ALL the files out\bench\%LATEST:~0,15%-shareable-part*.zip
explorer /select,"%~dp0out\bench\%LATEST%"
goto done
:noexe
echo Missing out\build\host\sfr_cpu_diagnostic.exe - this folder is not a complete benchmark kit.
:done
echo.
pause
