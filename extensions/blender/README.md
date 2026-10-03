# YAPF for Blender

Import and export `.yapf` images in **Blender 4.2 or later** (tested with
Blender 5.1).

- **File → Import → YAPF Image (.yapf)** — optionally as a **plane** with a
  material that shows the image.
- **Image Editor → Image → Open YAPF… / Save as YAPF…** (with mipmaps option).
- **Drag and drop** `.yapf` files into the 3D Viewport (creates a plane), the
  Image Editor or a node editor.

Blender cannot read `.yapf` on its own, so imported images are **packed into
the .blend** and survive saving and reopening.

## Install

1. Get `yapf_image-blender.zip` from the
   [Releases](https://github.com/Nexoniarz/YAPF/releases), or build it (below).
2. Blender → **Edit → Preferences → Get Extensions → ⌄ → Install from Disk…**
   and pick the zip.

The zip contains the native YAPF library for Windows, macOS and Linux (x64
and ARM64).  If no library matches your system, the extension still
**imports** using a pure-Python decoder (numpy, bundled with Blender), but
cannot export.

## Build the zip

```sh
python3 extensions/blender/build.py            # library for this machine only
tools/build_release.sh                         # all platforms (needs zig)
```

Both write `dist/yapf_image-blender.zip`.
