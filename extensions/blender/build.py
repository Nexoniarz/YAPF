#!/usr/bin/env python3
"""
Package the Blender extension:  python3 extensions/blender/build.py [libs_dir]

Writes dist/yapf_image-blender.zip, ready for Blender's
Edit > Preferences > Get Extensions > Install from Disk.

libs_dir (optional) holds prebuilt libraries as <platform>/<file>, e.g.
linux-x64/libyapf.so, windows-x64/yapf.dll, macos-arm64/libyapf.dylib
(tools/build_libs.sh makes them).  Without libraries the extension still
imports .yapf files using numpy, but cannot export.  If no libs_dir is
given, a library for this machine is built with `cc` when available.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SRC = os.path.join(ROOT, "extensions", "blender", "yapf_image")
sys.path.insert(0, os.path.join(ROOT, "bindings", "python"))
import yapf  # noqa: E402  (for the platform folder name)


def host_lib(tmp):
    name = {"win32": "yapf.dll", "darwin": "libyapf.dylib"}.get(sys.platform, "libyapf.so")
    out = os.path.join(tmp, yapf._platform_dir(), name)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc:
        return None
    cmd = [cc, "-O2", "-std=c99", "-shared", "-fPIC", os.path.join(ROOT, "yapf.c"), "-o", out, "-pthread"]
    return tmp if subprocess.call(cmd) == 0 else None


def main():
    tmp = tempfile.mkdtemp()
    libs = sys.argv[1] if len(sys.argv) > 1 else host_lib(tmp)
    os.makedirs(os.path.join(ROOT, "dist"), exist_ok=True)
    out = os.path.join(ROOT, "dist", "yapf_image-blender.zip")
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for f in ("__init__.py", "blender_manifest.toml"):
            z.write(os.path.join(SRC, f), f)
        z.write(os.path.join(ROOT, "bindings", "python", "yapf.py"), "yapf.py")
        z.write(os.path.join(ROOT, "LICENSE"), "LICENSE")
        if libs:
            for plat in sorted(os.listdir(libs)):
                for f in sorted(os.listdir(os.path.join(libs, plat))):
                    if f.endswith((".so", ".dll", ".dylib")):
                        z.write(os.path.join(libs, plat, f), "lib/%s/%s" % (plat, f))
                        print("  bundled lib/%s/%s" % (plat, f))
    shutil.rmtree(tmp, ignore_errors=True)
    print("wrote", out)


if __name__ == "__main__":
    main()
