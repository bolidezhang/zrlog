@echo off
setlocal EnableDelayedExpansion
rem Build the cross-library benchmark on Windows (MSVC).
rem
rem Third-party libraries are NOT vendored in this repo. Point ZRLOG_BENCH_DEPS
rem at a directory that contains:
rem     spdlog\include\   (gabime/spdlog v1.x)
rem     fmtlog\           (MengRao/fmtlog, with fmtlog.cc)
rem NanoLog is Linux-only and is skipped here.
rem
rem Override ZRLOG_BENCH_DEPS / ZRLOG_VCVARS if your paths differ.

if not defined ZRLOG_BENCH_DEPS set "ZRLOG_BENCH_DEPS=D:\work\zrlog-bench"
if not defined ZRLOG_VCVARS set "ZRLOG_VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"

if not exist "%ZRLOG_VCVARS%" (
    echo ERROR: vcvars64.bat not found: "%ZRLOG_VCVARS%"
    echo        Set ZRLOG_VCVARS to your Visual Studio installation.
    exit /b 1
)
if not exist "%ZRLOG_BENCH_DEPS%\spdlog\include" (
    echo ERROR: spdlog not found under "%ZRLOG_BENCH_DEPS%"
    echo        Set ZRLOG_BENCH_DEPS to the directory holding spdlog\ and fmtlog\.
    exit /b 1
)

call "%ZRLOG_VCVARS%" >nul 2>&1
cd /d "%~dp0.."
set "FLAGS=-nologo -utf-8 -std:c++17 -O2 -EHsc -MD -DNDEBUG -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS"
set "DEPS=%ZRLOG_BENCH_DEPS%"
set "OUT=bench\bin"
if not exist "%OUT%" mkdir "%OUT%"

echo [1/5] zrlog
cl %FLAGS% -I. -Ifmt\include bench\bench_zrlog.cpp -Fe:%OUT%\bench_zrlog.exe -Fo:%OUT%\ > "%TEMP%\zb.log" 2>&1
if errorlevel 1 (type "%TEMP%\zb.log" & exit /b 1)

echo [2/5] spdlog
cl %FLAGS% -Ibench -I%DEPS%\spdlog\include bench\bench_spdlog.cpp -Fe:%OUT%\bench_spdlog.exe -Fo:%OUT%\ > "%TEMP%\zb.log" 2>&1
if errorlevel 1 (type "%TEMP%\zb.log" & exit /b 1)

echo [3/5] fmtlog
cl %FLAGS% -DFMT_HEADER_ONLY -Ibench -I%DEPS%\fmtlog -Ifmt\include bench\bench_fmtlog.cpp %DEPS%\fmtlog\fmtlog.cc -Fe:%OUT%\bench_fmtlog.exe -Fo:%OUT%\ > "%TEMP%\zb.log" 2>&1
if errorlevel 1 (type "%TEMP%\zb.log" & exit /b 1)

echo [4/5] quill
if exist "%DEPS%\quill\include" (
    cl %FLAGS% -Ibench -I"%DEPS%\quill\include" bench\bench_quill.cpp -Fe:%OUT%\bench_quill.exe -Fo:%OUT%\ > "%TEMP%\zb.log" 2>&1
    if errorlevel 1 (type "%TEMP%\zb.log" & exit /b 1)
) else (
    echo   skipped: quill not found under "%DEPS%\quill"
)

echo [5/5] nanolog skipped (Linux only)
echo ALL_DONE
