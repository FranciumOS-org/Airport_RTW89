@echo off
setlocal EnableExtensions
rem Windows: double-click to put AirPort_RTW89 into your OpenCore EFI.
rem Runs "Kext Installer.py" with Python 3, installing Python first if it is
rem missing (winget, else the installer from python.org). The EFI partition is
rem only reachable as administrator, so that is asked for first.
net session >nul 2>&1
if errorlevel 1 (
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)
cd /d "%~dp0"

call :find_python
if defined PYEXE goto run

echo Python 3 is needed to edit config.plist, and it is not installed.
choice /c YN /m "Install it now (Python 3.12 from python.org)"
if errorlevel 2 goto no_python
call :install_python
call :find_python
if not defined PYEXE goto no_python

:run
"%PYEXE%" %PYOPT% "Kext Installer.py"
exit /b

:no_python
echo.
echo No Python 3. Install it from https://www.python.org/downloads/
echo (tick "Add python.exe to PATH"), then double-click "Kext Installer.cmd" again.
pause
exit /b 1

:find_python
set "PYEXE="
set "PYOPT="
rem the py launcher (python.org's installer puts it in C:\Windows)
py -3 -c "import sys" >nul 2>&1
if not errorlevel 1 (
    set "PYEXE=py"
    set "PYOPT=-3"
    exit /b
)
rem python on PATH, but not the Microsoft Store's stand-in, which only prints
rem "Python was not found": running it is the test
python -c "import sys; sys.exit(sys.version_info[0] != 3)" >nul 2>&1
if not errorlevel 1 (
    set "PYEXE=python"
    exit /b
)
rem installed a moment ago, before PATH was updated in this window
for /d %%D in ("%ProgramFiles%\Python3*" "%LocalAppData%\Programs\Python\Python3*") do (
    if exist "%%~D\python.exe" set "PYEXE=%%~D\python.exe"
)
exit /b

:install_python
where winget >nul 2>&1
if not errorlevel 1 (
    echo Installing Python 3.12 with winget...
    winget install -e --id Python.Python.3.12 --scope machine --silent --accept-package-agreements --accept-source-agreements
    call :find_python
    if defined PYEXE exit /b
)
echo Downloading the Python 3.12 installer from python.org...
powershell -NoProfile -Command "$ErrorActionPreference='Stop'; $f=Join-Path $env:TEMP 'python-3.12.10-amd64.exe'; Invoke-WebRequest -UseBasicParsing 'https://www.python.org/ftp/python/3.12.10/python-3.12.10-amd64.exe' -OutFile $f; Start-Process -Wait $f -ArgumentList '/quiet','InstallAllUsers=1','PrependPath=1','Include_launcher=1'"
exit /b
