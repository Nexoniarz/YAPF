# Development shell for building the plugin on NixOS: nix-shell kde-shell.nix
with import <nixpkgs> {};
mkShell {
  nativeBuildInputs = [ cmake kdePackages.extra-cmake-modules pkg-config ];
  buildInputs = [ kdePackages.kio kdePackages.qtbase kdePackages.kcoreaddons ];
}
