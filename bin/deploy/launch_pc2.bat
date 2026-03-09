@echo off
setlocal EnableDelayedExpansion
REM Bridge Command - PC2 Launcher (Secondary, 2 monitors)
REM Instances: radar (secondary), ecdis (secondary)
REM Place in C:\BridgeCommand\
REM PC1 must be running first (primary sends scenario data)

set BC_ROOT=%~dp0
set BC_EXE=%BC_ROOT%bridgecommand-bc.exe

if not exist "%BC_EXE%" (
    echo ERROR: bridgecommand-bc.exe not found
    pause
    exit /b 1
)

echo ===== Bridge Command PC2 (Secondary) =====
echo Waiting for PC1 primary to be running...
echo.

REM Launch radar
echo [1/2] Launching RADAR (secondary)
start "" /D "%BC_ROOT%" "%BC_EXE%" -c "radar\bc5.ini" --wicked
timeout /t 3 /nobreak >nul

REM Launch ecdis
echo [2/2] Launching ECDIS (secondary)
start "" /D "%BC_ROOT%" "%BC_EXE%" -c "ecdis\bc5.ini" --wicked

echo.
echo All 2 instances launched.
pause
