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
