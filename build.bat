@echo off
setlocal enabledelayedexpansion
:: ============================================================
::  YAPF build script — Clang / LLVM only
::  Windows x64 and ARM64
:: ============================================================
::
::  Usage:
::    build.bat [options]
::
::  Options:
::    --debug        Debug build (no optimisation, full symbols)
::    --simd         Enable SIMD paths (-DYAPF_USE_SIMD)
::    --static-only  Build only static libraries
::    --shared-only  Build only shared libraries (DLL)
::    --help         Show this message
::
::  Requirements:
::    clang       from https://github.com/llvm/llvm-project/releases
::                or installed via: winget install LLVM.LLVM
::    llvm-ar     bundled with LLVM
::    llvm-dlltool bundled with LLVM  (generates .lib import library)
::    lld-link    bundled with LLVM   (linker, used via clang -fuse-ld=lld)
::
::  All tools must be on PATH.  The LLVM installer adds them automatically
::  when "Add LLVM to the system PATH" is selected.
:: ============================================================

:: ── ANSI colour support (Windows 10 1511+, Windows Terminal) ────────
for /f "tokens=4-5 delims=. " %%i in ('ver') do set WIN_VER=%%i.%%j
set ANSI_OK=0
if !WIN_VER! GEQ 10.0 set ANSI_OK=1

if !ANSI_OK!==1 (
    set "C_RED=[31m"
    set "C_GRN=[32m"
    set "C_YLW=[33m"
    set "C_CYN=[36m"
    set "C_BLD=[1m"
    set "C_RST=[0m"
) else (
    set "C_RED=" & set "C_GRN=" & set "C_YLW="
    set "C_CYN=" & set "C_BLD=" & set "C_RST="
)

:: ── Option parsing ───────────────────────────────────────────────────
set OPT_DEBUG=0
set OPT_SIMD=0
set OPT_STATIC_ONLY=0
set OPT_SHARED_ONLY=0

for %%A in (%*) do (
    if /i "%%A"=="--debug"       set OPT_DEBUG=1
    if /i "%%A"=="--simd"        set OPT_SIMD=1
    if /i "%%A"=="--static-only" set OPT_STATIC_ONLY=1
    if /i "%%A"=="--shared-only" set OPT_SHARED_ONLY=1
    if /i "%%A"=="--help"        goto :show_help
)

if !OPT_STATIC_ONLY!==1 if !OPT_SHARED_ONLY!==1 (
    echo !C_RED![fail]!C_RST!  --static-only and --shared-only are mutually exclusive.
    exit /b 1
)
goto :after_help

:show_help
    findstr /n "^" "%~f0" | findstr /r "^[3-9]:" | findstr /r "^1[0-9]:" > nul
    more +2 "%~f0" | findstr /r "^::" | findstr /v "^:: ====" | ^
        for /f "delims=:" %%L in ('findstr /n "^"') do echo %%L
    exit /b 0

:after_help

:: ── Tool detection ───────────────────────────────────────────────────
echo.
echo !C_BLD!──── Detecting tools ────!C_RST!

where clang >nul 2>&1
if errorlevel 1 (
    echo !C_RED![fail]!C_RST!  clang not found.
    echo        Install LLVM: winget install LLVM.LLVM
    echo        Then add it to PATH and reopen this terminal.
    exit /b 1
)
for /f "delims=" %%V in ('clang --version 2^>^&1 ^| findstr /r "clang version"') do (
    echo !C_CYN![info]!C_RST!  Compiler : %%V
)

where llvm-ar >nul 2>&1
if errorlevel 1 (
    echo !C_RED![fail]!C_RST!  llvm-ar not found. Make sure LLVM is fully installed.
    exit /b 1
)
echo !C_CYN![info]!C_RST!  Archiver : llvm-ar (LLVM)

set HAVE_DLLTOOL=0
where llvm-dlltool >nul 2>&1
if not errorlevel 1 (
    set HAVE_DLLTOOL=1
    echo !C_CYN![info]!C_RST!  DllTool  : llvm-dlltool (LLVM)
) else (
    echo !C_YLW![warn]!C_RST!  llvm-dlltool not found — .lib import library will be skipped
)

:: ── Compiler flags ───────────────────────────────────────────────────
set CFLAGS=-std=c99 -Wall -Wextra -Wpedantic
set LFLAGS=-fuse-ld=lld

if !OPT_DEBUG!==1 (
    set CFLAGS=!CFLAGS! -O0 -g
    echo !C_CYN![info]!C_RST!  Mode     : debug
) else (
    set CFLAGS=!CFLAGS! -O3 -DNDEBUG
    set LFLAGS=!LFLAGS! -Wl,/OPT:REF,/OPT:ICF
    echo !C_CYN![info]!C_RST!  Mode     : release
)

if !OPT_SIMD!==1 (
    set CFLAGS=!CFLAGS! -DYAPF_USE_SIMD
    echo !C_CYN![info]!C_RST!  SIMD     : enabled
)

set BUILD_SHARED=1
set BUILD_STATIC=1
if !OPT_STATIC_ONLY!==1 set BUILD_SHARED=0
if !OPT_SHARED_ONLY!==1 set BUILD_STATIC=0

:: ── Output directories ───────────────────────────────────────────────
if not exist dist\include      mkdir dist\include
if not exist dist\windows-x64  mkdir dist\windows-x64
if not exist dist\windows-arm64 mkdir dist\windows-arm64

copy /y yapf.h dist\include\yapf.h >nul
echo !C_GRN![ ok ]!C_RST!  Header   ^→ dist\include\yapf.h

:: ── Write module definition file ─────────────────────────────────────
:: Used by llvm-dlltool to generate a MSVC-compatible import library.
(
    echo LIBRARY yapf
    echo EXPORTS
    echo     yapf_load
    echo     yapf_save
    echo     yapf_free
) > dist\_yapf.def

:: ====================================================================
::  Build: Windows x64
:: ====================================================================
echo.
echo !C_BLD!──── Building Windows x64 ────!C_RST!

if !BUILD_SHARED!==1 (
    clang --target=x86_64-w64-windows-gnu !CFLAGS! !LFLAGS! ^
        -shared yapf.c -o dist\windows-x64\yapf.dll
    if errorlevel 1 (
        echo !C_RED![fail]!C_RST!  Windows x64 shared build failed.
        del /q dist\_yapf.def 2>nul
        exit /b 1
    )
    echo !C_GRN![ ok ]!C_RST!  dist\windows-x64\yapf.dll

    if !HAVE_DLLTOOL!==1 (
        llvm-dlltool -m i386:x86-64 -D yapf.dll ^
            -d dist\_yapf.def -l dist\windows-x64\yapf.lib
        if not errorlevel 1 (
            echo !C_GRN![ ok ]!C_RST!  dist\windows-x64\yapf.lib  (import library)
        ) else (
            echo !C_YLW![warn]!C_RST!  Import library generation failed (non-fatal)
        )
    )
)

if !BUILD_STATIC!==1 (
    clang --target=x86_64-w64-windows-gnu !CFLAGS! ^
        -c yapf.c -o dist\windows-x64\_yapf.obj
    if errorlevel 1 (
        echo !C_RED![fail]!C_RST!  Windows x64 static compile failed.
        del /q dist\_yapf.def dist\windows-x64\_yapf.obj 2>nul
        exit /b 1
    )
    llvm-ar rcs dist\windows-x64\libyapf.a dist\windows-x64\_yapf.obj
    del /q dist\windows-x64\_yapf.obj
    echo !C_GRN![ ok ]!C_RST!  dist\windows-x64\libyapf.a  (static)
)

:: ====================================================================
::  Build: Windows ARM64
:: ====================================================================
echo.
echo !C_BLD!──── Building Windows ARM64 ────!C_RST!

:: Probe whether Clang can target ARM64 Windows without errors.
echo int x; | clang --target=aarch64-w64-windows-gnu -x c - -c -o nul >nul 2>&1
if errorlevel 1 (
    echo !C_YLW![warn]!C_RST!  ARM64 target not available in this Clang build — skipping.
    echo        If you need ARM64, install a full LLVM release from llvm.org.
    goto :skip_arm64
)

if !BUILD_SHARED!==1 (
    clang --target=aarch64-w64-windows-gnu !CFLAGS! !LFLAGS! ^
        -shared yapf.c -o dist\windows-arm64\yapf.dll
    if errorlevel 1 (
        echo !C_YLW![warn]!C_RST!  Windows ARM64 shared build failed — skipping.
        goto :skip_arm64
    )
    echo !C_GRN![ ok ]!C_RST!  dist\windows-arm64\yapf.dll

    if !HAVE_DLLTOOL!==1 (
        llvm-dlltool -m arm64 -D yapf.dll ^
            -d dist\_yapf.def -l dist\windows-arm64\yapf.lib
        if not errorlevel 1 (
            echo !C_GRN![ ok ]!C_RST!  dist\windows-arm64\yapf.lib  (import library)
        )
    )
)

if !BUILD_STATIC!==1 (
    clang --target=aarch64-w64-windows-gnu !CFLAGS! ^
        -c yapf.c -o dist\windows-arm64\_yapf.obj
    if not errorlevel 1 (
        llvm-ar rcs dist\windows-arm64\libyapf.a dist\windows-arm64\_yapf.obj
        del /q dist\windows-arm64\_yapf.obj
        echo !C_GRN![ ok ]!C_RST!  dist\windows-arm64\libyapf.a  (static)
    ) else (
        echo !C_YLW![warn]!C_RST!  Windows ARM64 static compile failed — skipping.
    )
)

:skip_arm64

:: ── Cleanup temp files ───────────────────────────────────────────────
del /q dist\_yapf.def 2>nul

:: ── Summary ──────────────────────────────────────────────────────────
echo.
echo !C_BLD!──── Summary ────!C_RST!
for /r dist %%F in (*) do (
    echo   !C_GRN!%%F!C_RST!
)
echo.
echo !C_GRN![ ok ]!C_RST!  Done.
endlocal
