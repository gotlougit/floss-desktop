{ pkgs, sources }:

let
  inherit (pkgs) lib;
  source = sources.pipewire;
  base = pkgs.pipewire.override { bluezSupport = false; };
  replacedOptions = [ "bluez5" "docs" "man" "installed_tests" "installed_test_prefix" ];
in
base.overrideAttrs (old: {
  pname = "pipewire-floss";
  version = "1.7.0-floss-${builtins.substring 0 12 source.rev}";
  outputs = [ "out" "jack" "dev" ];
  src = pkgs.fetchFromGitLab {
    domain = "gitlab.freedesktop.org";
    owner = "pipewire";
    repo = "pipewire";
    inherit (source) rev;
    hash = "sha256-zGrNtWPQqeimEEE2425FjcpUsVv/d68VMFXeEMrKqf8=";
  };
  # Keep the Nix-specific JACK lookup fix; upstream compatibility patches for
  # the nixpkgs release must not be applied to this independently pinned tree.
  patches = [ (builtins.head old.patches) ../patches/pipewire-floss.patch ];
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
  buildInputs = builtins.filter
    (input: !builtins.elem input [ pkgs.ldacbt pkgs.modemmanager ])
    old.buildInputs;
  nativeCheckInputs = [ ];
  doCheck = false;
  doInstallCheck = false;
  # Expose only checks for this source when explicitly added later.
  passthru = builtins.removeAttrs old.passthru [ "tests" ];
})
