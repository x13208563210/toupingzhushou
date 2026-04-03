@echo off
setlocal

set "EXE=%~dp0build\windows-receiver\Debug\AndroidCastReceiver.exe"

if exist "%EXE%" (
    start "" "%EXE%"
    goto :eof
)

echo Receiver executable not found. Please build the Windows receiver first.
pause
