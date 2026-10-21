@echo off
REM Compatibility shim: old scripts call handeye.exe / handeye <command>.
REM Everything is now in yx.exe; forward the same arguments.
"%~dp0yx.exe" %*
exit /b %ERRORLEVEL%
