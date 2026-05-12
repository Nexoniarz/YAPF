@echo off

REM ZIG IS REQUIRED TO COMPILE
REM Pikture Build Script - Fast, Small, Easy

if not exist dist\windows_x64 mkdir dist\windows_x64
if not exist dist\windows_arm64 mkdir dist\windows_arm64
if not exist dist\macos_x64 mkdir dist\macos_x64
if not exist dist\macos_arm64 mkdir dist\macos_arm64
if not exist dist\linux_x64 mkdir dist\linux_x64
if not exist dist\linux_arm64 mkdir dist\linux_arm64

set FLAGS=-O3 -fPIC

REM Windows (Shared + Import Lib)
zig cc -target x86_64-windows %FLAGS% -shared pikture.c -o dist\windows_x64\pikture.dll -Wl,--out-implib,dist\windows_x64\pikture.lib
zig cc -target aarch64-windows %FLAGS% -shared pikture.c -o dist\windows_arm64\pikture.dll -Wl,--out-implib,dist\windows_arm64\pikture.lib

REM macOS (Shared)
zig cc -target x86_64-macos %FLAGS% -shared pikture.c -o dist\macos_x64\libpikture.dylib
zig cc -target aarch64-macos %FLAGS% -shared pikture.c -o dist\macos_arm64\libpikture.dylib

REM Linux (Shared)
zig cc -target x86_64-linux-gnu %FLAGS% -shared pikture.c -o dist\linux_x64\libpikture.so
zig cc -target aarch64-linux-gnu %FLAGS% -shared pikture.c -o dist\linux_arm64\libpikture.so

REM Linux (Static)
zig cc -target x86_64-linux-musl %FLAGS% -c pikture.c -o pikture_x64.o
zig ar rcs dist\linux_x64\libpikture.a pikture_x64.o

zig cc -target aarch64-linux-musl %FLAGS% -c pikture.c -o pikture_arm64.o
zig ar rcs dist\linux_arm64\libpikture.a pikture_arm64.o

del /q *.o
