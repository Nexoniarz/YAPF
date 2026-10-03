# YAPF Image Preview for VS Code

Click any `.yapf` file in VS Code to see the image.

- Checkerboard behind transparent pixels.
- **Click** to switch between *fit to window* and *100 %*; **Ctrl + scroll**
  to zoom (pixels stay sharp when enlarged).
- Picker for every **mip level** stored in the file.
- Info bar: size, channels, sRGB / linear, GPU format hint, file size and
  compression ratio, decode time.
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

Works in VS Code, VSCodium and other VS Code–based editors (1.75+).
