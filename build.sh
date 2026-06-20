#!/usr/bin/env bash
# ============================================================
#  YAPF build script — Clang / LLVM only
# ============================================================
#
#  Usage:
#    ./build.sh [options]
#
#  Options:
#    --debug        Debug build: no optimisation, full symbols
#    --simd         Enable SIMD paths (-DYAPF_USE_SIMD)
#    --static-only  Build only static libraries
#    --shared-only  Build only shared libraries
#    --help         Show this message
#
#  macOS  →  produces universal (arm64 + x86_64) .dylib and .a
#  Linux  →  produces native .so + .a; attempts arm64 cross-compile
#             if clang supports the aarch64-linux-gnu target and
#             lld is available.  Install the cross sysroot with:
#               Debian/Ubuntu: sudo apt install gcc-aarch64-linux-gnu
#               Fedora:        sudo dnf install gcc-aarch64-linux-gnu
# ============================================================

set -euo pipefail

# ── ANSI colours (silenced when stdout is not a terminal) ────────────
if [[ -t 1 ]]; then
    RED=$'\033[0;31m' GRN=$'\033[0;32m' YLW=$'\033[1;33m'
    CYN=$'\033[0;36m' BLD=$'\033[1m'    RST=$'\033[0m'
else
    RED='' GRN='' YLW='' CYN='' BLD='' RST=''
fi

# ── Helpers ──────────────────────────────────────────────────────────
info()  { echo "${CYN}[info]${RST}  $*"; }
ok()    { echo "${GRN}[ ok ]${RST}  $*"; }
warn()  { echo "${YLW}[warn]${RST}  $*" >&2; }
die()   { echo "${RED}[fail]${RST}  $*" >&2; exit 1; }
step()  { echo; echo "${BLD}──── $* ────${RST}"; }

# ── Option parsing ───────────────────────────────────────────────────
OPT_DEBUG=0
OPT_SIMD=0
OPT_STATIC_ONLY=0
OPT_SHARED_ONLY=0

for arg in "$@"; do
    case "$arg" in
        --debug)       OPT_DEBUG=1 ;;
        --simd)        OPT_SIMD=1 ;;
        --static-only) OPT_STATIC_ONLY=1 ;;
        --shared-only) OPT_SHARED_ONLY=1 ;;
        --help)
            sed -n '3,30p' "$0" | sed 's/^#  \?//'
            exit 0
            ;;
        *) die "Unknown option: $arg  (try --help)" ;;
    esac
done

if [[ $OPT_STATIC_ONLY -eq 1 && $OPT_SHARED_ONLY -eq 1 ]]; then
    die "--static-only and --shared-only are mutually exclusive"
fi

# ── Tool detection ───────────────────────────────────────────────────
step "Detecting tools"

require() {
    command -v "$1" &>/dev/null || die "$1 not found. $2"
}

require clang "Install LLVM: https://releases.llvm.org/"

CLANG=$(command -v clang)
CLANG_VER=$("$CLANG" --version | head -1)
info "Compiler : $CLANG_VER"

# Prefer LLVM's archiver; fall back to the system one.
if   command -v llvm-ar &>/dev/null; then AR=$(command -v llvm-ar)
elif command -v ar       &>/dev/null; then AR=$(command -v ar)
else die "No archiver found (llvm-ar or ar)"
fi
info "Archiver : $AR"

STRIP=""
if   command -v llvm-strip &>/dev/null; then STRIP=$(command -v llvm-strip)
elif command -v strip       &>/dev/null; then STRIP=$(command -v strip)
fi
[[ -n "$STRIP" ]] && info "Strip    : $STRIP"

# ── Compiler flags ───────────────────────────────────────────────────
CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -fPIC"
LFLAGS=""

if [[ $OPT_DEBUG -eq 1 ]]; then
    CFLAGS="$CFLAGS -O0 -g"
    info "Mode     : debug"
else
    CFLAGS="$CFLAGS -O3 -DNDEBUG"
    LFLAGS="$LFLAGS -Wl,-s"   # strip symbols from shared libs
    info "Mode     : release"
fi

[[ $OPT_SIMD -eq 1 ]] && { CFLAGS="$CFLAGS -DYAPF_USE_SIMD"; info "SIMD     : enabled"; }

# ── Detect OS ────────────────────────────────────────────────────────
OS=$(uname -s)
HOST_ARCH=$(uname -m)
info "Host     : $OS / $HOST_ARCH"

# ── Output layout ────────────────────────────────────────────────────
mkdir -p dist/include

cp yapf.h dist/include/yapf.h
ok "Header   → dist/include/yapf.h"

BUILD_SHARED=$([[ $OPT_STATIC_ONLY -eq 0 ]] && echo 1 || echo 0)
BUILD_STATIC=$([[ $OPT_SHARED_ONLY -eq 0 ]] && echo 1 || echo 0)

# ── Build helpers ────────────────────────────────────────────────────

# compile_shared <target-triple> <out-path>
compile_shared() {
    local triple="$1" out="$2"
    "$CLANG" $CFLAGS $LFLAGS --target="$triple" -shared yapf.c -o "$out"
}

# compile_static <target-triple> <out-path>
compile_static() {
    local triple="$1" out="$2" obj
    obj="${out%.a}.o"
    "$CLANG" $CFLAGS --target="$triple" -c yapf.c -o "$obj"
    "$AR" rcs "$out" "$obj"
    rm -f "$obj"
}

# ── macOS build ──────────────────────────────────────────────────────
if [[ "$OS" == "Darwin" ]]; then

    require lipo "lipo is part of Xcode Command Line Tools: xcode-select --install"
    LIPO=$(command -v lipo)

    mkdir -p dist/macos

    step "Building macOS arm64"
    if [[ $BUILD_SHARED -eq 1 ]]; then
        compile_shared arm64-apple-macos11     /tmp/_yapf_arm64.dylib
        ok "arm64 dylib"
    fi
    if [[ $BUILD_STATIC -eq 1 ]]; then
        compile_static arm64-apple-macos11     /tmp/_yapf_arm64.a
        ok "arm64 static"
    fi

    step "Building macOS x86_64"
    if [[ $BUILD_SHARED -eq 1 ]]; then
        compile_shared x86_64-apple-macos10.15 /tmp/_yapf_x64.dylib
        ok "x86_64 dylib"
    fi
    if [[ $BUILD_STATIC -eq 1 ]]; then
        compile_static x86_64-apple-macos10.15 /tmp/_yapf_x64.a
        ok "x86_64 static"
    fi

    step "Creating universal binaries (lipo)"
    if [[ $BUILD_SHARED -eq 1 ]]; then
        "$LIPO" -create /tmp/_yapf_arm64.dylib /tmp/_yapf_x64.dylib \
                -output dist/macos/libyapf.dylib
        rm -f /tmp/_yapf_arm64.dylib /tmp/_yapf_x64.dylib
        ok "dist/macos/libyapf.dylib  (universal)"
    fi
    if [[ $BUILD_STATIC -eq 1 ]]; then
        "$LIPO" -create /tmp/_yapf_arm64.a /tmp/_yapf_x64.a \
                -output dist/macos/libyapf.a
        rm -f /tmp/_yapf_arm64.a /tmp/_yapf_x64.a
        ok "dist/macos/libyapf.a      (universal)"
    fi

# ── Linux build ──────────────────────────────────────────────────────
elif [[ "$OS" == "Linux" ]]; then

    # Normalise arch name to the triple component clang expects.
    case "$HOST_ARCH" in
        x86_64)  NATIVE_TRIPLE="x86_64-linux-gnu"  ;;
        aarch64) NATIVE_TRIPLE="aarch64-linux-gnu"  ;;
        armv7*)  NATIVE_TRIPLE="armv7-linux-gnueabihf" ;;
        *)       NATIVE_TRIPLE="$HOST_ARCH-linux-gnu" ;;
    esac

    # Friendly dir name (x86_64 → linux-x64, aarch64 → linux-arm64, etc.)
    case "$HOST_ARCH" in
        x86_64)  NATIVE_DIR="linux-x64"   ;;
        aarch64) NATIVE_DIR="linux-arm64"  ;;
        *)       NATIVE_DIR="linux-$HOST_ARCH" ;;
    esac

    mkdir -p "dist/$NATIVE_DIR"

    step "Building Linux $HOST_ARCH (native)"
    if [[ $BUILD_SHARED -eq 1 ]]; then
        compile_shared "$NATIVE_TRIPLE" "dist/$NATIVE_DIR/libyapf.so"
        ok "dist/$NATIVE_DIR/libyapf.so"
    fi
    if [[ $BUILD_STATIC -eq 1 ]]; then
        compile_static "$NATIVE_TRIPLE" "dist/$NATIVE_DIR/libyapf.a"
        ok "dist/$NATIVE_DIR/libyapf.a"
    fi

    # ── Cross-compile to the other common arch ────────────────────────
    if [[ "$HOST_ARCH" == "x86_64" ]]; then
        CROSS_TRIPLE="aarch64-linux-gnu"
        CROSS_DIR="linux-arm64"
    elif [[ "$HOST_ARCH" == "aarch64" ]]; then
        CROSS_TRIPLE="x86_64-linux-gnu"
        CROSS_DIR="linux-x64"
    else
        CROSS_TRIPLE=""
    fi

    if [[ -n "$CROSS_TRIPLE" ]]; then
        step "Cross-compiling Linux ${CROSS_DIR##linux-} (${CROSS_TRIPLE})"

        # Probe by actually compiling a file that exercises the C standard
        # library headers — a bare "int x;" succeeds even with a broken
        # sysroot because it never touches any include paths.
        CROSS_PROBE_SRC=$'#include <stdio.h>\nint x;\n'
        CROSS_PROBE_OK=0
        if printf '%s' "$CROSS_PROBE_SRC" | \
               "$CLANG" --target="$CROSS_TRIPLE" -fuse-ld=lld \
               -x c - -c -o /dev/null 2>/dev/null; then
            CROSS_PROBE_OK=1
        fi

        if [[ $CROSS_PROBE_OK -eq 0 ]]; then
            warn "Cross-sysroot for ${CROSS_TRIPLE} not available — skipping."
            warn "To enable: install the cross toolchain for your distro:"
            warn "  Debian/Ubuntu:  sudo apt install gcc-aarch64-linux-gnu lld"
            warn "  Fedora:         sudo dnf install gcc-aarch64-linux-gnu lld"
            warn "  Arch:           sudo pacman -S aarch64-linux-gnu-gcc lld"
        else
            mkdir -p "dist/$CROSS_DIR"
            # Disable set -e locally so a cross-compile failure is non-fatal.
            set +e
            CROSS_BUILD_OK=1

            if [[ $BUILD_SHARED -eq 1 ]]; then
                "$CLANG" $CFLAGS $LFLAGS --target="$CROSS_TRIPLE" -fuse-ld=lld \
                    -shared yapf.c -o "dist/$CROSS_DIR/libyapf.so" 2>/tmp/_yapf_cross_err
                if [[ $? -eq 0 ]]; then
                    ok "dist/$CROSS_DIR/libyapf.so"
                else
                    warn "Shared cross-compile failed:"; cat /tmp/_yapf_cross_err >&2
                    CROSS_BUILD_OK=0
                fi
            fi

            if [[ $BUILD_STATIC -eq 1 ]]; then
                "$CLANG" $CFLAGS --target="$CROSS_TRIPLE" -fuse-ld=lld \
                    -c yapf.c -o /tmp/_yapf_cross.o 2>/tmp/_yapf_cross_err
                if [[ $? -eq 0 ]]; then
                    "$AR" rcs "dist/$CROSS_DIR/libyapf.a" /tmp/_yapf_cross.o
                    rm -f /tmp/_yapf_cross.o
                    ok "dist/$CROSS_DIR/libyapf.a"
                else
                    warn "Static cross-compile failed:"; cat /tmp/_yapf_cross_err >&2
                    CROSS_BUILD_OK=0
                fi
            fi

            rm -f /tmp/_yapf_cross_err
            set -e

            if [[ $CROSS_BUILD_OK -eq 0 ]]; then
                warn "Cross-compile produced partial results — check warnings above."
            fi
        fi
    fi

else
    die "Unsupported OS: $OS.  Use build.bat on Windows."
fi

# ── Summary ──────────────────────────────────────────────────────────
step "Summary"
find dist -type f | sort | while IFS= read -r f; do
    size=$(du -sh "$f" 2>/dev/null | cut -f1)
    printf "  ${GRN}%-44s${RST} %s\n" "$f" "$size"
done
echo
ok "Done."
