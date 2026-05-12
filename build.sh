#!/bin/bash

# ZIG IS REQUIRED TO COMPILE
# Pikture Build Script - Fast, Small, Easy

mkdir -p dist/windows_x64 dist/windows_arm64 dist/macos_x64 dist/macos_arm64 dist/linux_x64 dist/linux_arm64

FLAGS="-O3 -fPIC"

# Windows (Shared + Import Lib)
zig cc -target x86_64-windows $FLAGS -shared pikture.c -o dist/windows_x64/pikture.dll -Wl,--out-implib,dist/windows_x64/pikture.lib
zig cc -target aarch64-windows $FLAGS -shared pikture.c -o dist/windows_arm64/pikture.dll -Wl,--out-implib,dist/windows_arm64/pikture.lib

# macOS (Shared)
zig cc -target x86_64-macos $FLAGS -shared pikture.c -o dist/macos_x64/libpikture.dylib
zig cc -target aarch64-macos $FLAGS -shared pikture.c -o dist/macos_arm64/libpikture.dylib

# Linux (Shared)
zig cc -target x86_64-linux-gnu $FLAGS -shared pikture.c -o dist/linux_x64/libpikture.so
zig cc -target aarch64-linux-gnu $FLAGS -shared pikture.c -o dist/linux_arm64/libpikture.so

# Linux (Static)
zig cc -target x86_64-linux-musl $FLAGS -c pikture.c -o pikture_x64.o
ar rcs dist/linux_x64/libpikture.a pikture_x64.o

zig cc -target aarch64-linux-musl $FLAGS -c pikture.c -o pikture_arm64.o
ar rcs dist/linux_arm64/libpikture.a pikture_arm64.o

rm *.o
