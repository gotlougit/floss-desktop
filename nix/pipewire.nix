{ pkgs }:

let
  inherit (pkgs) lib;
  upstream = pkgs.pipewire.override { bluezSupport = false; };
  replacedOptions = [ "docs" "man" "tests" "installed_tests" "installed_test_prefix" ];
in
# Keep nixpkgs' upstream source and compatibility patches. Floss supplies
# Bluetooth audio through the external pw-floss client, so the BlueZ backend
# and build-only documentation/test dependencies are omitted.
upstream.overrideAttrs (old: {
  outputs = [ "out" "jack" "dev" ];
  mesonFlags = builtins.filter
    (flag: !lib.any (option: lib.hasPrefix "-D${option}" flag) replacedOptions)
    old.mesonFlags ++ [
      "-Ddocs=disabled"
      "-Dman=disabled"
      "-Dtests=disabled"
      "-Dinstalled_tests=disabled"
      "-Dexamples=disabled"
    ];
  nativeBuildInputs = builtins.filter
    (input: !builtins.elem input [ pkgs.doxygen pkgs.graphviz pkgs.docutils ])
    old.nativeBuildInputs;
  nativeCheckInputs = [ ];
  doCheck = false;
  doInstallCheck = false;
  # Expose only checks for this source when explicitly added later.
  passthru = builtins.removeAttrs old.passthru [ "tests" ];
})
