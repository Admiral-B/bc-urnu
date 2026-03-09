@echo off
setlocal EnableDelayedExpansion
REM Bridge Command - PC1 Launcher (Master, 3 TVs)
REM Instances: center (primary), port (secondary), starboard (secondary)
REM Place in C:\BridgeCommand\

set BC_ROOT=%~dp0
set BC_EXE=%BC_ROOT%bridgecommand-bc.exe

if not exist "%BC_EXE%" (
    echo ERROR: bridgecommand-bc.exe not found
    pause
    exit /b 1
)

echo ===== Bridge Command PC1 (Master) =====
echo.

REM Launch center (primary) first -- it runs the simulation
echo [1/3] Launching CENTER (primary, look_angle=0)
start "" /D "%BC_ROOT%" "%BC_EXE%" -c "center\bc5.ini" --wicked
echo   Waiting 10s for primary to initialize...
timeout /t 10 /nobreak >nul

REM Launch port
echo [2/3] Launching PORT (secondary, look_angle=-90)
start "" /D "%BC_ROOT%" "%BC_EXE%" -c "port\bc5.ini" --wicked
timeout /t 3 /nobreak >nul

REM Launch starboard
echo [3/3] Launching STARBOARD (secondary, look_angle=90)
start "" /D "%BC_ROOT%" "%BC_EXE%" -c "starboard\bc5.ini" --wicked

echo.
echo All 3 instances launched.
pause
