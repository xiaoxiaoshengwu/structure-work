@echo off
setlocal
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_all.ps1"
set "TRUSS_EXIT=%ERRORLEVEL%"
pause
exit /b %TRUSS_EXIT%
