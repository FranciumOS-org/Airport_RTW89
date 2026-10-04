@echo off
rem Windows: double-click to set up AirPort_RTW89 in your OpenCore EFI (setup.py).
rem The EFI partition is only reachable as administrator: ask for that first.
net session >nul 2>&1
if errorlevel 1 (
    powershell -NoProfile -Command "Start-Process -FilePath %~f0 -Verb RunAs"
    exit /b
)
cd /d "%~dp0"
where py >nul 2>&1
if not errorlevel 1 (
    py -3 setup.py
    goto :eof
)
where python >nul 2>&1
if not errorlevel 1 (
    python setup.py
    goto :eof
)
echo Python 3 is needed: https://www.python.org/downloads/ (tick "Add python.exe to PATH"),
echo then double-click setup.cmd again.
pause
