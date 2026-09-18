{
  description = "Floss-only Bluetooth daemon and Plasma/PipeWire integration";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/dc5d91f840324650bac8c379428c7037a416959a";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
      sources = builtins.fromJSON (builtins.readFile ./patches/sources.json);
    in {
      nixosModules.default = { pkgs, ... }@args: import ./nix/module.nix {
        packages = self.packages.${pkgs.stdenv.hostPlatform.system};
      } args;
      checks = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
          report = import ./nix/eval-module.nix {
            inherit nixpkgs;
            packages = self.packages.${system};
            module = self.nixosModules.default;
          };
        in {
          module-evaluation =
            assert report.failedAssertions == [];
            assert !report.bluezService;
            # This is an evaluation report, not a request to realize every
            # store path mentioned by the evaluated system configuration.
            pkgs.writeText "floss-module-evaluation.json"
              (builtins.unsafeDiscardStringContext (builtins.toJSON report));
        });
      packages = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
          call = file: import file { inherit pkgs sources; };
        in rec {
          pipewire = call ./nix/pipewire.nix;
          bluedevil = call ./nix/bluedevil.nix;
          floss = call ./nix/floss.nix;
          wireplumber = import ./nix/wireplumber.nix { inherit pkgs sources pipewire; };
          stack = pkgs.linkFarm "floss-desktop-stack" [
            { name = "floss"; path = floss; }
            { name = "pipewire"; path = pipewire; }
            { name = "wireplumber"; path = wireplumber; }
            { name = "bluedevil"; path = bluedevil; }
          ];
          default = stack;
        });
    };
}
