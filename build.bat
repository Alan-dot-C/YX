@echo off
REM ===========================================================================
REM  build.bat - build handeye.exe + yx.exe
REM
REM  Requires the Visual Studio "Desktop development with C++" workload.
REM  If cl.exe is not on PATH, run this from the
REM  "x64 Native Tools Command Prompt for VS" instead.
REM
REM  Note: keep this file pure ASCII. cmd.exe reads .bat as the OEM codepage,
REM  so UTF-8 Chinese comments here get mangled and break parsing.
REM ===========================================================================
setlocal

where cl >nul 2>nul
if errorlevel 1 (
    echo [ERROR] cl.exe not found.
    echo         Open "x64 Native Tools Command Prompt for VS" and run this again,
    echo         or call vcvars64.bat first.
    exit /b 1
)

rem Separate object directories: the two targets share some sources, and /Fo
rem with a plain prefix is rejected when there is more than one source file.
if not exist build\handeye mkdir build\handeye
if not exist build\yx mkdir build\yx

echo Building handeye.exe ...
rem /MP: compile the translation units in parallel (cuts a full build from ~35s to ~10s)
cl /nologo /utf-8 /std:c++20 /EHsc /O2 /MT /MP /Fo:build\handeye\ /Fe:handeye.exe src\*.cpp windowsapp.lib gdi32.lib user32.lib ole32.lib

if errorlevel 1 (
    echo [FAILED]
    exit /b 1
)

echo Building yx.exe ...
rem Only the core the server needs today; eye/sieve join in the next step.
cl /nologo /utf-8 /std:c++20 /EHsc /O2 /MT /MP /Fo:build\yx\ /Fe:yx.exe src\common.cpp src\log.cpp src\yx\main.cpp user32.lib advapi32.lib

if errorlevel 1 (
    echo [FAILED]
    exit /b 1
)

echo [OK] handeye.exe + yx.exe
exit /b 0
