@echo off
setlocal
cd /d "%~dp0"
if not exist "%~dp0truss_ui.exe" goto missing
if not exist "%~dp0truss.exe" goto missing
start "" "%~dp0truss_ui.exe"
exit /b 0
:missing
echo Please keep truss_ui.exe and truss.exe in the same program folder.
pause
exit /b 1
