@echo off
REM Bridge Command - Generate bc5.ini for a specific instance
REM Usage: generate_ini.bat <output_dir> <mode> <look_angle> <master_ip> [extra_args]
REM   mode: primary | secondary
REM   look_angle: degrees (0=ahead, -90=port, 90=starboard, 180=astern)
REM   master_ip: IP of the primary instance (only used by primary to list secondaries)

set OUTPUT_DIR=%~1
set MODE=%~2
set LOOK_ANGLE=%~3
set MASTER_IP=%~4

if "%OUTPUT_DIR%"=="" (
    echo Usage: generate_ini.bat ^<output_dir^> ^<mode^> ^<look_angle^> ^<master_ip^>
    exit /b 1
)

set INI_FILE=%OUTPUT_DIR%\bc5.ini

REM Copy template
copy /Y "%~dp0..\bc5.ini" "%INI_FILE%" >nul

REM Set secondary_mode
if /i "%MODE%"=="secondary" (
    powershell -Command "(Get-Content '%INI_FILE%') -replace 'secondary_mode=0', 'secondary_mode=1' | Set-Content '%INI_FILE%'"
)

REM Set look_angle
powershell -Command "(Get-Content '%INI_FILE%') -replace 'look_angle=0', 'look_angle=%LOOK_ANGLE%' | Set-Content '%INI_FILE%'"

REM Set use_wicked_engine=1 (DX12)
powershell -Command "(Get-Content '%INI_FILE%') -replace 'use_wicked_engine=0', 'use_wicked_engine=1' | Set-Content '%INI_FILE%'"

REM Fullscreen borderless
powershell -Command "(Get-Content '%INI_FILE%') -replace 'graphics_mode=2', 'graphics_mode=3' | Set-Content '%INI_FILE%'"

echo Generated %INI_FILE% (mode=%MODE% look_angle=%LOOK_ANGLE%)
