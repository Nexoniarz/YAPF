# Linux desktop integration

Makes file managers recognise `.yapf` files (MIME type `image/x-yapf`) and
show **thumbnails** for them.

```sh
sh extensions/linux/install.sh             # for your user
sudo sh extensions/linux/install.sh --system
```

Thumbnails are made by `yapf thumbnail` (from the `yapf` tool, which must be
on your `PATH` — `sudo make install` puts it in `/usr/local/bin`).  They work
in file managers that use freedesktop thumbnailers: GNOME Files (Nautilus),
Nemo, Caja, and Thunar (with tumbler).  KDE Dolphin uses its own plugin
system and is not covered yet.

Thumbnails cover every YAPF file the tool reads: version 1 and 2, GPU
blocks (BC1–BC7, BC6H), 16-bit and float textures, and the first image of
cubes, arrays and animations.  KDE Dolphin: see [../kde](../kde).

### NixOS

`/usr/share` is not used on NixOS.  Install the package from the
repository's `default.nix` instead — it carries the MIME type and a
thumbnailer that calls `yapf` by its full store path:

```nix
# configuration.nix
environment.systemPackages = [ (pkgs.callPackage /path/to/YAPF { }) ];
environment.pathsToLink = [ "/share/thumbnailers" ];   # if your desktop does not link it already
```

For your user only: `nix-env -if /path/to/YAPF`, then
`sh extensions/linux/install.sh` (the per-user install).
