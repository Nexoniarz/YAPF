#!/bin/sh
# Registers the YAPF MIME type and thumbnailer for the current user, so file
# managers that use freedesktop thumbnailers (GNOME Files, Nemo, Caja,
# Thunar with tumbler) show .yapf previews.  Needs `yapf` on your PATH.
#   sh extensions/linux/install.sh            (current user)
#   sudo sh extensions/linux/install.sh --system
set -e
here=$(cd "$(dirname "$0")" && pwd)
if [ "$1" = "--system" ]; then data=/usr/share; else data=${XDG_DATA_HOME:-$HOME/.local/share}; fi

command -v yapf >/dev/null 2>&1 || echo "warning: 'yapf' is not on your PATH; thumbnails need it" >&2

mkdir -p "$data/mime/packages" "$data/thumbnailers"
cp "$here/yapf-mime.xml" "$data/mime/packages/yapf.xml"
cp "$here/yapf.thumbnailer" "$data/thumbnailers/yapf.thumbnailer"
update-mime-database "$data/mime" >/dev/null 2>&1 || true
echo "Installed the image/x-yapf MIME type and thumbnailer into $data"
echo "Restart your file manager (e.g. 'nautilus -q') and clear ~/.cache/thumbnails/fail if needed."
