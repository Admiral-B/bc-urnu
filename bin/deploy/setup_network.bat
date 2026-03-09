@echo off
REM Bridge Command - Network Setup Script
REM Run as Administrator on EACH bridge PC
REM Sets up firewall rules and static IP for Bridge Command networking

echo Bridge Command Network Setup
echo ============================
echo.

REM Check admin privileges
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo ERROR: Run this script as Administrator
    pause
    exit /b 1
)

REM Get PC role
set /p ROLE="Enter role (master/secondary): "
if /i "%ROLE%"=="master" (
    set IP=192.168.1.10
) else if /i "%ROLE%"=="secondary" (
    set IP=192.168.1.11
) else (
    echo Invalid role. Use 'master' or 'secondary'.
    pause
    exit /b 1
)

echo.
echo Setting static IP: %IP%
netsh interface ip set address "Ethernet" static %IP% 255.255.255.0

echo.
echo Adding firewall rules...
netsh advfirewall firewall add rule name="BC ENet UDP" protocol=udp dir=in localport=18300-18400 action=allow
netsh advfirewall firewall add rule name="Allow Ping" protocol=icmpv4:8,any dir=in action=allow
netsh advfirewall firewall add rule name="BC SMB" dir=in action=allow protocol=tcp localport=445

echo.
echo Setting network profile to Private...
powershell -Command "Set-NetConnectionProfile -InterfaceAlias 'Ethernet' -NetworkCategory Private"

echo.
echo Enabling file sharing...
netsh advfirewall firewall set rule group="File and Printer Sharing" new enable=Yes

echo.
echo Sharing C drive for deployment...
net share CDrive=C:\ /grant:everyone,full 2>nul
if %errorlevel% neq 0 echo   (Share may already exist - OK)

echo.
echo Enabling guest access for file sharing...
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Lsa" /v everyoneincludesanonymous /t REG_DWORD /d 1 /f
reg add "HKLM\SYSTEM\CurrentControlSet\Services\LanmanWorkstation\Parameters" /v AllowInsecureGuestAuth /t REG_DWORD /d 1 /f
reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System" /v LocalAccountTokenFilterPolicy /t REG_DWORD /d 1 /f
net user guest /active:yes 2>nul

echo.
echo ============================
echo Network setup complete.
echo IP: %IP%
echo.
echo Ping test from the other PC:
echo   ping %IP%
echo.
echo You may need to REBOOT for registry changes to take full effect.
echo ============================
pause
