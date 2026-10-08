@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"
truss.exe "..\总控信息\必做题_总控信息.txt" "..\计算结果\必做题_机算结果.txt"
if errorlevel 1 goto failed
truss.exe "..\总控信息\选做题_总控信息.txt" "..\计算结果\选做题_机算结果.txt"
if errorlevel 1 goto failed
echo 两题计算完成，结果已保存到“计算结果”目录。
pause
exit /b 0
:failed
echo 计算失败，请查看以上错误信息。
pause
exit /b 2
