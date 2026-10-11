# Development shell for building the plugin on NixOS: nix-shell gimp-shell.nix
# `gimp3` is GIMP 3 on every channel (on unstable it is an alias of `gimp`).
with import <nixpkgs> {};
mkShell {
  nativeBuildInputs = [ pkg-config ];
  buildInputs = [ gimp3 gimp3.dev gtk3 glib gdk-pixbuf gegl babl cairo pango ];
}
