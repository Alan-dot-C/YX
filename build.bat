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
rem Organs only: collect + execute. No sieve, no ledger, no verification.
cl /nologo /utf-8 /std:c++20 /EHsc /O2 /MT /MP /Fo:build\handeye\ /Fe:handeye.exe ^
    src\common.cpp src\log.cpp src\eye.cpp src\uia.cpp src\winenum.cpp src\clip.cpp ^
    src\input.cpp src\mouth.cpp src\schema.cpp src\doctor.cpp src\main.cpp ^
    windowsapp.lib gdi32.lib user32.lib ole32.lib advapi32.lib shell32.lib

if errorlevel 1 (
    echo [FAILED]
    exit /b 1
)

echo Building yx.exe ...
rem v0.1: one capture, L0/L1/L2 projections, zoom.
cl /nologo /utf-8 /std:c++20 /EHsc /O2 /MT /MP /Fo:build\yx\ /Fe:yx.exe ^
    src\common.cpp src\log.cpp src\sieve.cpp src\eye.cpp src\uia.cpp src\winenum.cpp ^
    src\yx\main.cpp src\yx\words.cpp ^
    windowsapp.lib gdi32.lib user32.lib ole32.lib oleaut32.lib advapi32.lib

if errorlevel 1 (
    echo [FAILED]
    exit /b 1
)

echo [OK] handeye.exe + yx.exe
exit /b 0
