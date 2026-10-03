# YAPF for GIMP

Open and export `.yapf` images in **GIMP 3.0 or later**.

- **File → Open** reads `.yapf` (Gray, Gray + Alpha, RGB, RGBA).
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
| Windows         | MSYS2: `pacman -S mingw-w64-ucrt-x86_64-gimp`        |
| macOS           | build GIMP 3 from source or use MacPorts/Homebrew   |

Then, from this folder:

```sh
gimptool --install yapf-gimp.c
```

That compiles the plugin (it pulls in `../../yapf.c` itself) and installs it
into your personal GIMP plug-ins folder.  Restart GIMP.

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
- Tested with GIMP 3.0.8 on Linux: opening matches the reference decoder
  pixel for pixel, and export equals GIMP's own flattened PNG of the image.
