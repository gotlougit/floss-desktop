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
    hash = "sha256-ZcTLtdLqJHc5lzldwI5SnxVAm4Ac0HOmgrHdXrtrc3c=";
  };
  patches = [ ../patches/wireplumber-floss.patch ];
  nativeBuildInputs = old.nativeBuildInputs ++ [ pkgs.python3 ];
  mesonFlags = old.mesonFlags ++ [ "-Dtests=false" "-Ddbus-tests=false" ];
  doCheck = false;
  doInstallCheck = false;
})
