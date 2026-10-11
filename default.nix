# YAPF for Nix / NixOS: the `yapf` tool, libyapf (static + shared) and
# yapf.h, plus the image/x-yapf MIME type and a freedesktop thumbnailer.
#
#   nix-build                 build ./result
#   nix-env -if .             install for your user
#   NixOS: environment.systemPackages = [ (pkgs.callPackage /path/to/YAPF { }) ];
{ pkgs ? import <nixpkgs> { }, lib ? pkgs.lib, stdenv ? pkgs.stdenv }:

stdenv.mkDerivation {
  pname = "yapf";
  version = "2.0";

  src = lib.cleanSourceWith {
    src = ./.;
    filter = path: type:
      let base = baseNameOf path; in
      !(lib.elem base [ "build" "dist" "result" ".git" "node_modules" ]);
  };

  makeFlags = [ "PREFIX=$(out)" ];
  doCheck = true;
  checkTarget = "test";

  postInstall = ''
    install -Dm644 extensions/linux/yapf-mime.xml $out/share/mime/packages/yapf.xml
    install -Dm644 extensions/linux/yapf.thumbnailer $out/share/thumbnailers/yapf.thumbnailer
    substituteInPlace $out/share/thumbnailers/yapf.thumbnailer \
      --replace-fail "TryExec=yapf" "TryExec=$out/bin/yapf" \
      --replace-fail "Exec=yapf " "Exec=$out/bin/yapf "
  '';

  meta = {
    description = "YAPF: fast lossless images and GPU-ready textures";
    homepage = "https://github.com/Nexoniarz/YAPF";
    license = lib.licenses.asl20;
    platforms = lib.platforms.unix;
    mainProgram = "yapf";
  };
}
