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
          usbReport = import ./nix/eval-module.nix {
            inherit nixpkgs;
            packages = self.packages.${system};
            module = self.nixosModules.default;
            usbTransport = true;
          };
        in {
          module-evaluation =
            assert report.failedAssertions == [];
            assert !report.bluezService;
            assert report.kernelPatches == [];
            assert builtins.elem "plasma-pa-microphone-lifecycle.patch" report.desktopAudioPatches.plasmaPa;
            # This is an evaluation report, not a request to realize every
            # store path mentioned by the evaluated system configuration.
            pkgs.writeText "floss-module-evaluation.json"
              (builtins.unsafeDiscardStringContext (builtins.toJSON report));
          usb-transport-module-evaluation =
            assert usbReport.failedAssertions == [];
            assert usbReport.kernelPatches == [];
            assert builtins.elem "hci_vhci" usbReport.usbTransport.kernelModules;
            assert builtins.elem "floss-usb.service" usbReport.usbTransport.managerRequires;
            pkgs.writeText "floss-usb-module-evaluation.json"
              (builtins.unsafeDiscardStringContext (builtins.toJSON usbReport.usbTransport));
        });
      packages = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
          call = file: import file { inherit pkgs sources; };
        in rec {
          pipewire = import ./nix/pipewire.nix { inherit pkgs; };
          plasma-pa = (import ./nix/desktop-audio-overlay.nix pkgs pkgs).kdePackages.plasma-pa;
          pw-floss = import ./nix/pw-floss.nix { inherit pkgs pipewire; };
          floss-usb = import ./nix/floss-usb.nix { inherit pkgs; };
          bluedevil = call ./nix/bluedevil.nix;
          floss = call ./nix/floss.nix;
          wireplumber = import ./nix/wireplumber.nix { inherit pkgs sources pipewire; };
          stack = pkgs.linkFarm "floss-desktop-stack" [
            { name = "floss"; path = floss; }
            { name = "pipewire"; path = pipewire; }
            { name = "pw-floss"; path = pw-floss; }
            { name = "wireplumber"; path = wireplumber; }
            { name = "bluedevil"; path = bluedevil; }
          ];
          default = stack;
        });
    };
}
