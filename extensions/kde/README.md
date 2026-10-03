# YAPF thumbnails for KDE (Dolphin)

Shows previews of `.yapf` files in **Dolphin**, the KDE file dialogs and
anything else that uses KIO thumbnails.  KDE Frameworks 6 / Plasma 6.
Files with mipmaps are thumbnailed from the nearest mip level, so even huge
textures preview instantly.

## Build and install

You need CMake, a C/C++ compiler and the KDE development packages:

| System          | Packages |
| :-------------- | :------- |
| Arch / Manjaro  | `sudo pacman -S cmake extra-cmake-modules kio` |
| Debian / Ubuntu | `sudo apt install cmake extra-cmake-modules libkf6kio-dev qt6-base-dev` |
| Fedora          | `sudo dnf install cmake extra-cmake-modules kf6-kio-devel qt6-qtbase-devel` |
| openSUSE        | `sudo zypper install cmake kf6-extra-cmake-modules kf6-kio-devel qt6-base-devel` |
| NixOS           | `nix-shell kde-shell.nix` (in this folder) |

```sh
cd extensions/kde
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build
update-mime-database /usr/share/mime
```

This installs the plugin (`kf6/thumbcreator/yapfthumbnail.so`) and the
`image/x-yapf` file type.  Then in Dolphin: **Settings → Configure Dolphin →
Interface → Previews**, tick **YAPF Images**, and turn previews on.

### Without root (per user, also NixOS)

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$HOME/.local
cmake --build build && cmake --install build
update-mime-database ~/.local/share/mime
mkdir -p ~/.config/plasma-workspace/env
echo 'export QT_PLUGIN_PATH="$HOME/.local/lib/plugins:$QT_PLUGIN_PATH"' > ~/.config/plasma-workspace/env/yapf.sh
```

Log out and back in, then enable **YAPF Images** in Dolphin's preview settings.
(Some distributions use `lib64/plugins` or `lib/qt6/plugins`; `cmake
--install` prints the exact folder.)

## Status

Built against KF6 / Qt 6 and tested by loading the plugin through KDE's
plugin factory and creating thumbnails (RGBA, RGB, mipmapped, wide images;
corrupt files are rejected).  Not yet tried inside a running Dolphin.
