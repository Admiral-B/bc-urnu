@echo off
setlocal EnableDelayedExpansion
REM Bridge Command - Launch all instances on this PC
REM Place this in C:\BridgeCommand\ alongside the instance directories
REM Each instance dir needs only a bc5.ini (exe/data shared from root)
REM Uses -c flag to point each instance at its own bc5.ini

echo Bridge Command Launcher
echo =======================
echo.

REM --- CONFIGURATION ---
REM Edit INSTANCES for this PC. First entry = launched first (should be primary).
REM PC1 (3 TVs):     set INSTANCES=center port starboard
REM PC2 (2 monitors): set INSTANCES=radar ecdis
set INSTANCES=center port starboard

REM Delay after first launch (seconds) - primary needs time to start
set PRIMARY_DELAY=8
REM --- END CONFIGURATION ---

set BC_ROOT=%~dp0..
set BC_EXE=%BC_ROOT%\bridgecommand-bc.exe

if not exist "%BC_EXE%" (
    echo ERROR: bridgecommand-bc.exe not found in %BC_ROOT%
    pause
    exit /b 1
)

set COUNT=0
for %%i in (%INSTANCES%) do (
    set /a COUNT+=1
    set "INI_PATH=%BC_ROOT%\%%i\bc5.ini"

    if not exist "!INI_PATH!" (
        echo ERROR: !INI_PATH! not found
        echo Create instance directory %%i with a bc5.ini first.
        pause
        exit /b 1
    )

    echo [!COUNT!] Launching: %%i
    echo     INI: %%i\bc5.ini
    start "" /D "%BC_ROOT%" "%BC_EXE%" -c "%%i\bc5.ini"

    REM After the first (primary) instance, wait before launching secondaries
    if !COUNT!==1 (
        echo     Waiting %PRIMARY_DELAY%s for primary to initialize...
        timeout /t %PRIMARY_DELAY% /nobreak >nul
    ) else (
        timeout /t 2 /nobreak >nul
    )
)

echo.
echo All %COUNT% instances launched.
pause
