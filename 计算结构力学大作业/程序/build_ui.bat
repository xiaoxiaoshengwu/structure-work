@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" -Target UI
set "TRUSS_BUILD_EXIT=%ERRORLEVEL%"
if /i not "%~1"=="/ui" pause
exit /b %TRUSS_BUILD_EXIT%
