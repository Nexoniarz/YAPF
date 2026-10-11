# YAPF Image Preview for VS Code

Click any `.yapf` file in VS Code to see the image.

- Checkerboard behind transparent pixels.
- **Click** to switch between *fit to window* and *100 %*; **Ctrl + scroll**
  to zoom (pixels stay sharp when enlarged).
- Opens every YAPF file, version 1 and 2: GPU blocks (BC1–BC7, BC6H)
  decoded, 16-bit and float textures shown in 8-bit (floats sRGB encoded).
- Pickers for **mip level**, cube **face**, array **layer**, animation
  **frame** and volume **slice**.
- Info bar: size, shape, storage (e.g. *GPU BC7*, *RGB half float*), sRGB /
  linear, normal-map convention, format version, file size, decode time.
  Damaged files say so (checksum mismatch).
- Reloads automatically when the file changes on disk.

Decoding runs in plain JavaScript inside the preview; no native code.

## Install

- From a release: download `yapf-preview.vsix`, then in VS Code run
  **Extensions: Install from VSIX…** (or `code --install-extension yapf-preview.vsix`).
- From source (needs Node.js):

  ```sh
  cd extensions/vscode
  npm run package          # → yapf-preview-1.0.0.vsix
  code --install-extension yapf-preview-1.0.0.vsix
  ```

Should work in VS Code 1.75+ and VS Code–based editors such as VSCodium.

## Status

The `.vsix` packages cleanly, and the preview code was tested in a browser
engine (headless Chromium) with real files: correct pixels, mip picker,
error message for corrupt files; the YAPF 2 preview was tested the same
way (BC7, BC5, BC6H, half float, 16-bit, cube, animation, damaged and
non-YAPF files).  **It has not yet been run inside VS Code itself.**
