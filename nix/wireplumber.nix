{ pkgs, sources, pipewire }:
(pkgs.wireplumber.override {
  inherit pipewire;
  enableDocs = false;
  enableGI = false;
}).overrideAttrs (old: {
  version = "0.5.17-floss-${builtins.substring 0 12 sources.wireplumber.rev}";
  src = pkgs.fetchFromGitLab {
    domain = "gitlab.freedesktop.org";
    owner = "pipewire";
    repo = "wireplumber";
    rev = sources.wireplumber.rev;
    hash = "sha256-jb+OrTVQ4d1hCX2TGWx5x+dvNS2kV4frBHLNk/0GnVo=";
  };
  patches = [ ../patches/wireplumber-floss.patch ];
  nativeBuildInputs = old.nativeBuildInputs ++ [ pkgs.python3 ];
  mesonFlags = old.mesonFlags ++ [ "-Dtests=false" "-Ddbus-tests=false" ];
  doCheck = false;
  doInstallCheck = false;
})
