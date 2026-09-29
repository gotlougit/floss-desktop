{ pkgs, sources, pipewire }:
(pkgs.wireplumber.override {
  inherit pipewire;
  enableDocs = false;
  enableGI = false;
}).overrideAttrs (old: {
  version = "0.5.18-floss-${builtins.substring 0 12 sources.wireplumber.rev}";
  src = pkgs.fetchFromGitLab {
    domain = "gitlab.freedesktop.org";
    owner = "pipewire";
    repo = "wireplumber";
    rev = sources.wireplumber.rev;
    hash = "sha256-VIa11ynYJsr4zqZ7XogPin4Ks0oEDltCjo51aWVMH8c=";
  };
  patches = [ ../patches/wireplumber-floss.patch ];
  nativeBuildInputs = old.nativeBuildInputs ++ [ pkgs.python3 ];
  mesonFlags = old.mesonFlags ++ [ "-Dtests=false" "-Ddbus-tests=false" ];
  doCheck = false;
  doInstallCheck = false;
})
