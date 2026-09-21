@echo off
rem Double-click wrapper for Install-Calculator.ps1.
rem
rem Asks for administrator once (the calc.exe redirection needs it), then hands
rem off to the script. Any arguments are passed straight through, so
rem   Install-Calculator.cmd -Action Uninstall
rem works the same as calling the script directly.

setlocal

net session >nul 2>&1
if not errorlevel 1 goto :elevated

echo Requesting administrator...
if "%~1"=="" (
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
) else (
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -ArgumentList '%*' -Verb RunAs"
)
exit /b 0

:elevated
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-Calculator.ps1" %*
set "RESULT=%ERRORLEVEL%"
echo.
pause
exit /b %RESULT%
