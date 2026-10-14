@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0flash.ps1" -Firmware flight %*
set FLASH_EXIT_CODE=%ERRORLEVEL%
if "%~1"=="" pause
exit /b %FLASH_EXIT_CODE%
