@echo off
rem Load self-test of the candleLight adapter in four settings: numbered
rem frames at 1000 a second, looped back inside the adapter, nothing goes to
rem the bus. Writes selftest.txt next to this file. Close ucan-monitor first.
cd /d "%~dp0"
echo Self-test of the adapter, about 1.5 minutes. Nothing goes to the bus.
echo selftest %DATE% %TIME% > selftest.txt
for %%o in ("" "-f 1" "-f 0 -r 0" "-P") do (
  echo.
  echo === settings: %%~o
  echo. >> selftest.txt
  echo === settings: %%~o >> selftest.txt
  candle-test.exe -m loopback -b 500000 -s 123#0000000000000000 -c -p 1 -t 20 %%~o >> selftest.txt 2>&1
)
echo.
type selftest.txt
echo.
echo Saved to %CD%\selftest.txt
pause
