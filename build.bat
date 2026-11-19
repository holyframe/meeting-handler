@echo off
setlocal
if /i "%~1"=="debug" (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" -Configuration Debug
) else (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
)
exit /b %ERRORLEVEL%
