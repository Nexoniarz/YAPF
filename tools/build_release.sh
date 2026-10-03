#!/usr/bin/env bash
# Builds every release download into dist/release/ — for maintainers.
#
#   tools/build_release.sh
#
# Needs: zig (cross-compiles all six platforms from one machine), python3,
# and node + npx for the VS Code package.  Outputs:
#   yapf-<platform>.zip        command-line tool + shared/static library + header
#   yapf_image-blender.zip     Blender extension with libraries for all platforms
#   yapf-preview.vsix          VS Code extension
#   yapf-figma.zip             Figma plugin (import via manifest.json)
#   yapf-photoshop.zip         Photoshop scripts
#   yapf-gimp-source.zip       GIMP plugin source (build with gimptool)
#   yapf-kde-source.zip        Dolphin thumbnail plugin source (build with CMake)
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=dist/release
LIBS=dist/libs
rm -rf "$OUT" "$LIBS"
mkdir -p "$OUT" "$LIBS"

command -v zig >/dev/null || { echo "zig not found: https://ziglang.org/download/" >&2; exit 1; }
export ZIG_GLOBAL_CACHE_DIR=${ZIG_GLOBAL_CACHE_DIR:-$PWD/dist/.zig-cache}

#        zig target              folder          library        tool
targets="x86_64-linux-gnu.2.17   linux-x64       libyapf.so     yapf
         aarch64-linux-gnu.2.17  linux-arm64     libyapf.so     yapf
         x86_64-windows-gnu      windows-x64     yapf.dll       yapf.exe
         aarch64-windows-gnu     windows-arm64   yapf.dll       yapf.exe
         x86_64-macos            macos-x64       libyapf.dylib  yapf
         aarch64-macos           macos-arm64     libyapf.dylib  yapf"

echo "$targets" | while read -r tgt dir lib tool; do
    pkg=dist/pkg/yapf-$dir
    rm -rf "$pkg" && mkdir -p "$pkg" "$LIBS/$dir"
    zig cc -target "$tgt" -O2 -std=c99 -shared -fPIC -s yapf.c -o "$LIBS/$dir/$lib"
    zig cc -target "$tgt" -O2 -std=c99 -s tools/yapf_cli.c yapf.c -o "$pkg/$tool" -lm
    zig cc -target "$tgt" -O2 -std=c99 -c yapf.c -o "$pkg/yapf.o"
    zig ar rcs "$pkg/libyapf.a" "$pkg/yapf.o" && rm "$pkg/yapf.o"
    cp "$LIBS/$dir/$lib" yapf.h LICENSE "$pkg/"
    rm -f "$pkg"/*.lib "$pkg"/*.pdb "$LIBS/$dir"/*.lib "$LIBS/$dir"/*.pdb
    (cd dist/pkg && python3 -m zipfile -c "../../$OUT/yapf-$dir.zip" "yapf-$dir")
    echo "built yapf-$dir.zip"
done

python3 extensions/blender/build.py "$LIBS"
mv dist/yapf_image-blender.zip "$OUT/"

if command -v npx >/dev/null; then
    (cd extensions/vscode && npm run --silent package && mv yapf-preview-*.vsix "../../$OUT/yapf-preview.vsix")
else
    echo "node/npx not found: skipped the VS Code package" >&2
fi

(cd extensions/figma && node build.js 2>/dev/null || true)
python3 -m zipfile -c "$OUT/yapf-figma.zip" extensions/figma/manifest.json extensions/figma/code.js extensions/figma/ui.html
python3 -m zipfile -c "$OUT/yapf-photoshop.zip" extensions/photoshop
python3 -m zipfile -c "$OUT/yapf-gimp-source.zip" extensions/gimp yapf.c yapf.h
python3 -m zipfile -c "$OUT/yapf-kde-source.zip" extensions/kde extensions/linux/yapf-mime.xml yapf.c yapf.h
ls -la "$OUT"
