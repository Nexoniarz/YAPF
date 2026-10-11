# YAPF for GIMP

Open and export `.yapf` images in **GIMP 3.0 or later**.

- **File → Open** reads every `.yapf` file, version 1 and 2: Gray, Gray +
  Alpha, RGB, RGBA in 8-bit, 16-bit, half and float precision; GPU blocks
  (BC1–BC7, BC6H HDR) decoded for viewing; cube faces, array layers, volume
  slices and animation frames as separate layers.  When a file cannot be
  opened, the message says why (damaged, truncated, not YAPF, newer format).
- **File → Export As… → `name.yapf`** writes the visible image (layers are
  flattened into a copy; your document is untouched), with an optional
  **Store mipmaps** checkbox for game engines.

## Install

You need GIMP 3 with its development files (`gimptool`).

| System          | Development files                                   |
| :-------------- | :-------------------------------------------------- |
| Arch / Manjaro  | `sudo pacman -S gimp` (includes them)               |
| Debian / Ubuntu | `sudo apt install libgimp-3.0-dev` (GIMP 3 packages) |
| Fedora          | `sudo dnf install gimp-devel`                       |
| NixOS           | `nix-shell gimp-shell.nix` (in this folder), then build inside that shell |
| Windows         | MSYS2: `pacman -S mingw-w64-ucrt-x86_64-gimp`        |
| macOS           | build GIMP 3 from source or use MacPorts/Homebrew   |

Then, from this folder:

```sh
gimptool --install yapf-gimp.c
```

That compiles the plugin (it pulls in `../../yapf.c` itself) and installs it
into your personal GIMP plug-ins folder.  Restart GIMP.

On NixOS, build with the same nixpkgs channel your GIMP comes from (the
plugin links against that GIMP's libraries), and build it again after a
GIMP update.  Your GIMP must be GIMP 3: `environment.systemPackages = [
pkgs.gimp3 ];`, or `nix-shell -p gimp3 --run gimp` to try it.

### Manual build

```sh
gcc -O2 $(pkg-config --cflags gimpui-3.0) yapf-gimp.c $(pkg-config --libs gimpui-3.0) -o yapf-gimp
```

Copy `yapf-gimp` to `<GIMP config>/plug-ins/yapf-gimp/yapf-gimp`, where the
GIMP config folder is `~/.config/GIMP/3.0` on Linux,
`%APPDATA%\GIMP\3.0` on Windows and
`~/Library/Application Support/GIMP/3.0` on macOS.

## Notes

- Pixels are saved with the sRGB flag, because GIMP's 8-bit buffers are
  gamma encoded.  Files marked linear load as linear.
- When a file has mipmaps, GIMP opens the full-size image.
- ETC2 / EAC / ASTC blocks cannot be shown yet.  Export writes 8-bit
  version 1 files, as before.
- Tested with GIMP 3.0.8 on Linux: opening matches the reference decoder
  pixel for pixel, and export equals GIMP's own flattened PNG of the image.
  The version 2 loader was tested in GIMP 3.0.6 (nixpkgs 25.05, headless):
  BC7, BC5, BC6H, half float, 16-bit, cube, animation and a damaged file.
