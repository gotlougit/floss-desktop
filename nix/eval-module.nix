# Evaluation only: no NixOS system build or activation.
{ nixpkgs, packages, module ? import ./module.nix { inherit packages; } }:
let
  system = import (nixpkgs + "/nixos/lib/eval-config.nix") {
    system = "x86_64-linux";
    modules = [
      module
      ({ ... }: {
        system.stateVersion = "26.05";
        hardware.bluetooth.enable = true;
        hardware.bluetooth.floss.users = [ "floss-desktop" ];
        users.users.floss-desktop.isNormalUser = true;
        services.pipewire.enable = true;
        services.pipewire.alsa.enable = true;
        services.pipewire.alsa.support32Bit = true;
        services.pipewire.jack.enable = true;
        services.pipewire.pulse.enable = true;
        services.pipewire.wireplumber.enable = true;
        services.desktopManager.plasma6.enable = true;
        fileSystems."/" = { device = "/dev/disk/by-label/nixos"; fsType = "ext4"; };
        boot.loader.grub.enable = false;
      })
    ];
  };
  cfg = system.config;
in {
  failedAssertions = map (a: a.message) (builtins.filter (a: !a.assertion) cfg.assertions);
  bluezService = builtins.hasAttr "bluetooth" cfg.systemd.services;
  manager = cfg.systemd.services.btmanagerd.serviceConfig;
  adapter = cfg.systemd.services."btadapterd@".serviceConfig;
  codec = cfg.systemd.services.floss-codec.serviceConfig;
  automaticAudio = cfg.systemd.user.services.floss-audio.serviceConfig;
  reconnectPolicy = cfg.systemd.user.services.floss-policy.serviceConfig;
  pairingAgent = cfg.systemd.user.services.floss-pairing.serviceConfig;
  interopDatabase = cfg.environment.etc."bluetooth/interop_database.conf".source;
  pipewire = cfg.services.pipewire.package.drvPath;
  wireplumber = cfg.services.pipewire.wireplumber.package.drvPath;
  systemPackages = map (p: p.name) cfg.environment.systemPackages;
  units = builtins.attrNames cfg.systemd.units;
  unitTexts = {
    "btmanagerd.service" = cfg.systemd.units."btmanagerd.service".text;
    "btadapterd@.service" = cfg.systemd.units."btadapterd@.service".text;
    "floss-codec.service" = cfg.systemd.units."floss-codec.service".text;
  };
  userUnitTexts = {
    "floss-audio.service" = cfg.systemd.user.units."floss-audio.service".text;
    "floss-policy.service" = cfg.systemd.user.units."floss-policy.service".text;
    "floss-pairing.service" = cfg.systemd.user.units."floss-pairing.service".text;
  };
}
