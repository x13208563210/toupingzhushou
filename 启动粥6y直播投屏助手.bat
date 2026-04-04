@echo off
setlocal

set "EXE=%~dp0build\windows-receiver\Release\AndroidCastReceiver.exe"

if exist "%EXE%" (
    start "" "%EXE%"
    goto :eof
)

echo 未找到 PC 接收端可执行文件，请先构建 Release 版本。
pause
