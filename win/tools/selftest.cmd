@echo off
setlocal
rem Loopback only: no traffic is sent onto the CAN bus.
cd /d "%~dp0"
set "duration=60"
if not "%~1"=="" set "duration=%~1"
echo Close ucan-monitor before testing. Nothing goes to the bus.
echo Duration: %duration% seconds. For a soak test: selftest.cmd 1800
echo selftest %DATE% %TIME% > selftest.txt
candle-test.exe -m loopback -b 500000 -s 123#0000000000000000 -c -p 1 -t %duration% >> selftest.txt 2>&1
set "result=%ERRORLEVEL%"
echo Exit code: %result% >> selftest.txt
type selftest.txt
echo Saved to %CD%\selftest.txt
pause
exit /b %result%
