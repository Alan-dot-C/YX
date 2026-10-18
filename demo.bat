@echo off
setlocal
cd /d "%~dp0"

start "YX" /min "%~dp0yx.exe" serve
timeout /t 1 /nobreak >nul

echo.
"%~dp0yx.exe" framing --level L0
echo.
pause
