@echo off
setlocal EnableDelayedExpansion
REM Bridge Command - Deploy to Remote PC
REM Run from the dev laptop after setup_network.bat has been run on target PCs
REM Copies bin/ contents to the remote PC and creates per-instance directories

echo Bridge Command Deployment
echo =========================
echo.

set /p TARGET_IP="Enter target PC IP (e.g. 192.168.1.10): "
echo.
echo Which PC is this?
echo   1 = PC1 (Master, 3 TVs: center + port + starboard)
echo   2 = PC2 (Secondary, 2 monitors: radar + ecdis)
set /p PC_ROLE="Enter 1 or 2: "

set REMOTE_BASE=\\%TARGET_IP%\CDrive\BridgeCommand
set LOCAL_BIN=%~dp0..

echo.
echo Target: %REMOTE_BASE%
echo Source: %LOCAL_BIN%
echo.

REM Test connectivity
ping -n 1 -w 1000 %TARGET_IP% >nul 2>&1
if %errorlevel% neq 0 (
    echo ERROR: Cannot reach %TARGET_IP%
    echo Make sure setup_network.bat was run on the target PC
    pause
    exit /b 1
)

REM Create base directory
echo Creating %REMOTE_BASE%...
mkdir %REMOTE_BASE% 2>nul

REM Copy shared files (exe, dll, data)
echo Copying executables and DLLs...
xcopy "%LOCAL_BIN%\*.exe" "%REMOTE_BASE%\" /Y /Q
xcopy "%LOCAL_BIN%\*.dll" "%REMOTE_BASE%\" /Y /Q

echo Copying data directories...
xcopy "%LOCAL_BIN%\Sounds" "%REMOTE_BASE%\Sounds\" /E /Y /Q /I
xcopy "%LOCAL_BIN%\Models" "%REMOTE_BASE%\Models\" /E /Y /Q /I
xcopy "%LOCAL_BIN%\World" "%REMOTE_BASE%\World\" /E /Y /Q /I
xcopy "%LOCAL_BIN%\Scenarios" "%REMOTE_BASE%\Scenarios\" /E /Y /Q /I
xcopy "%LOCAL_BIN%\shaders" "%REMOTE_BASE%\shaders\" /E /Y /Q /I 2>nul
if exist "%LOCAL_BIN%\Data" xcopy "%LOCAL_BIN%\Data" "%REMOTE_BASE%\Data\" /E /Y /Q /I
if exist "%LOCAL_BIN%\media" xcopy "%LOCAL_BIN%\media" "%REMOTE_BASE%\media\" /E /Y /Q /I

REM Copy launch scripts
echo Copying launch scripts...
if "%PC_ROLE%"=="1" (
    copy "%LOCAL_BIN%\deploy\launch_pc1.bat" "%REMOTE_BASE%\launch_bridge.bat" /Y >nul
) else (
    copy "%LOCAL_BIN%\deploy\launch_pc2.bat" "%REMOTE_BASE%\launch_bridge.bat" /Y >nul
)

REM Create per-instance directories with preconfigured bc5.ini
echo.
echo Creating instance directories...

if "%PC_ROLE%"=="1" (
    REM PC1: center (primary, look_angle=0), port (secondary, -90), starboard (secondary, +90)
    call :create_instance "%REMOTE_BASE%" center 0 0
    call :create_instance "%REMOTE_BASE%" port 1 -90
    call :create_instance "%REMOTE_BASE%" starboard 1 90
) else (
    REM PC2: radar (secondary, look_angle=0), ecdis (secondary, look_angle=180)
    call :create_instance "%REMOTE_BASE%" radar 1 0
    call :create_instance "%REMOTE_BASE%" ecdis 1 180
)

echo.
echo =========================
echo Deployment complete.
echo.
echo To start: run launch_bridge.bat on the target PC
echo   (PC1 must start first, then PC2)
echo =========================
pause
exit /b 0

:create_instance
REM %1 = base dir, %2 = instance name, %3 = secondary_mode, %4 = look_angle
set BASE=%~1
set NAME=%~2
set SEC_MODE=%~3
set ANGLE=%~4

mkdir "%BASE%\%NAME%" 2>nul
copy "%LOCAL_BIN%\bc5.ini" "%BASE%\%NAME%\bc5.ini" /Y >nul

REM Configure secondary_mode
powershell -Command "(Get-Content '%BASE%\%NAME%\bc5.ini') -replace 'secondary_mode=0', 'secondary_mode=%SEC_MODE%' | Set-Content '%BASE%\%NAME%\bc5.ini'"

REM Configure look_angle
powershell -Command "(Get-Content '%BASE%\%NAME%\bc5.ini') -replace 'look_angle=0', 'look_angle=%ANGLE%' | Set-Content '%BASE%\%NAME%\bc5.ini'"

REM Force WickedEngine and fullscreen
powershell -Command "(Get-Content '%BASE%\%NAME%\bc5.ini') -replace 'use_wicked_engine=0', 'use_wicked_engine=1' | Set-Content '%BASE%\%NAME%\bc5.ini'"
powershell -Command "(Get-Content '%BASE%\%NAME%\bc5.ini') -replace 'graphics_mode=2', 'graphics_mode=3' | Set-Content '%BASE%\%NAME%\bc5.ini'"

echo   %NAME%: secondary_mode=%SEC_MODE% look_angle=%ANGLE%
goto :eof
