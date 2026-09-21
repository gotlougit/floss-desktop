# Import through this flake's nixosModules.default. This replaces the stock
# Bluetooth module, while retaining hardware.bluetooth.enable as the switch.
{ packages }:
{ config, lib, pkgs, ... }:
let
  inherit (lib) mkEnableOption mkOption mkIf types;
  cfg = config.hardware.bluetooth;
  floss = cfg.package;
  # Floss's Linux loader expects this static database under /var/lib. Extract
  # it separately so the runtime closure retains the file, not the source tree.
  interopDatabase = pkgs.runCommand "floss-interop-database" {} ''
    install -Dm644 ${floss.src}/system/conf/interop_database.conf $out/interop_database.conf
  '';
  allowedCallbacks = [
    "ManagerCallback" "BluetoothCallback" "BluetoothConnectionCallback"
    "BluetoothMediaCallback" "BluetoothTelephonyCallback" "BatteryProviderCallback"
    "QACallback" "BatteryManagerCallback" "BluetoothGattCallback"
    "BluetoothGattServerCallback" "ScannerCallback" "AdvertisingSetCallback"
    "SocketManagerCallback" "SuspendCallback" "AdminPolicyCallback"
  ];
  policy = pkgs.writeTextDir "share/dbus-1/system.d/floss.conf" ''
    <!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN"
      "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
    <busconfig>
      <policy user="floss">
        <allow own="org.chromium.bluetooth"/>
        <allow own="org.chromium.bluetooth.Manager"/>
        <allow send_destination="org.chromium.mmc.CodecManager" send_interface="org.chromium.mmc.CodecManager" send_member="CodecInit"/>
        <allow send_destination="org.chromium.mmc.CodecManager" send_interface="org.chromium.mmc.CodecManager" send_member="CodecCleanUp"/>
        ${lib.concatMapStringsSep "\n" (name: ''
          <allow send_interface="org.chromium.bluetooth.${name}" send_type="method_call"/>
        '') allowedCallbacks}
      </policy>
      <policy user="floss-codec">
        <allow own="org.chromium.mmc.CodecManager"/>
      </policy>
      <policy group="bluetooth">
        <allow send_destination="org.chromium.bluetooth"/>
        <allow send_destination="org.chromium.bluetooth.Manager"/>
      </policy>
      <policy group="bluetooth-audio">
        <allow own="org.pipewire.FlossAudio"/>
        <allow send_destination="org.chromium.bluetooth"/>
      </policy>
      <policy user="root">
        <allow send_destination="org.chromium.bluetooth"/>
        <allow send_destination="org.chromium.bluetooth.Manager"/>
      </policy>
    </busconfig>
  '';
  common = {
    User = "floss";
    Group = "floss";
    SupplementaryGroups = [ "bluetooth" "bluetooth-audio" ];
    StateDirectory = "bluetooth";
    StateDirectoryMode = "0700";
    LogsDirectory = "bluetooth";
    LogsDirectoryMode = "0700";
    UMask = "0007";
    NoNewPrivileges = true;
    ProtectSystem = "strict";
    ProtectHome = true;
    PrivateTmp = true;
    ProtectKernelTunables = true;
    ProtectKernelModules = true;
    ProtectKernelLogs = true;
    ProtectControlGroups = true;
    DevicePolicy = "closed";
    RestrictSUIDSGID = true;
    RestrictRealtime = true;
    LockPersonality = true;
    RestrictNamespaces = true;
    SystemCallArchitectures = "native";
    # Match the pinned NixOS BlueZ service's executable-memory, syscall and
    # process-visibility restrictions. Floss's native code has no JIT; the
    # manager's PID watcher only needs processes owned by this same user.
    MemoryDenyWriteExecute = true;
    SystemCallFilter = [ "@system-service" ];
    ProtectProc = "invisible";
    RestrictAddressFamilies = [ "AF_UNIX" "AF_BLUETOOTH" "AF_NETLINK" ];
    CapabilityBoundingSet = [ "CAP_NET_ADMIN" "CAP_NET_RAW" ];
    AmbientCapabilities = [ "CAP_NET_ADMIN" "CAP_NET_RAW" ];
    ReadWritePaths = [ "/run/bluetooth" ];
    TimeoutStopSec = 30;
    Restart = "on-failure";
    RestartSec = 2;
  };
  adapterLauncher = pkgs.writeShellScript "floss-adapter" ''
    set -eu
    instance="$1"
    case "$instance" in
      ${toString cfg.floss.adapter}_${toString cfg.floss.adapter}) ;;
      *) echo "Unsupported Floss adapter instance: $instance" >&2; exit 1 ;;
    esac
    virtual="''${instance%%_*}"
    physical="''${instance#*_}"
    ${lib.optionalString cfg.floss.usbTransport.enable ''
      packet_size=$(cat /run/floss-usb/sco-packet-size)
      case "$packet_size" in 0|24|60) ;; *) echo "Invalid USB SCO packet size" >&2; exit 1 ;; esac
      export FLOSS_HFP_SOFTWARE_HCI_TRANSPORT=true
      export FLOSS_HFP_MSBC_PACKET_SIZE="$packet_size"
    ''}
    exec ${floss}/bin/btadapterd --index="$virtual" --hci="$physical" --log-output=stderr
  '';
in {
  disabledModules = [ "services/hardware/bluetooth.nix" ];
  # Plasma adds these packages outside its excludePackages list. Substitute the
  # Floss UI and omit the obsolete BlueZ/OBEX desktop tools when enabled.
  options.environment.systemPackages = mkOption {
    apply = installed: if !cfg.enable then installed else
      lib.unique (map (package: if lib.getName package == "bluedevil" then packages.bluedevil else package)
        (builtins.filter (package: !builtins.elem (lib.getName package)
          [ "bluez" "bluez-qt" "openobex" "obexftp" ]) installed));
  };
  options.hardware.bluetooth = {
    enable = mkEnableOption "the Floss Bluetooth stack (replacing BlueZ)";
    package = mkOption { type = types.package; default = packages.floss; description = "Patched Floss daemon package."; };
    hsphfpd.enable = mkOption { type = types.bool; default = false; description = "Compatibility option; hsphfpd cannot be enabled with Floss."; };
    powerOnBoot = mkOption { type = types.bool; default = true; description = "Initial enabled state when no saved Floss manager configuration exists."; };
    floss = {
      adapter = mkOption { type = types.enum [ 0 ]; default = 0; description = "Initial deployment supports only virtual adapter 0 backed by hci0."; };
      users = mkOption { type = types.listOf types.str; default = []; description = "Local users granted Bluetooth control and PCM access; membership is not seat-scoped."; };
      softwareHciTransport = mkEnableOption "software HCI SCO transport, only for a controller/kernel known to support it";
      usbTransport.enable = mkEnableOption "the experimental userspace USB/VHCI transport for one Realtek 0bda:c123 Bluetooth adapter";
    };
  };

  config = mkIf cfg.enable {
    assertions = [ { assertion = !cfg.hsphfpd.enable; message = "Floss provides HFP; hsphfpd must remain disabled."; } ];
    users.groups.floss = {};
    users.groups.floss-codec = {};
    users.groups.bluetooth = {};
    users.groups.bluetooth-audio = {};
    users.groups.floss-usb = mkIf cfg.floss.usbTransport.enable {};
    users.users = (lib.genAttrs cfg.floss.users (_: {
      extraGroups = [ "bluetooth" "bluetooth-audio" ];
    })) // lib.optionalAttrs cfg.floss.usbTransport.enable {
      floss-usb = { isSystemUser = true; group = "floss-usb"; };
    } // {
      floss = {
        isSystemUser = true;
        group = "floss";
        extraGroups = [ "bluetooth" "bluetooth-audio" "floss-codec" ] ++ lib.optional cfg.floss.usbTransport.enable "floss-usb";
      };
      floss-codec = {
        isSystemUser = true;
        group = "floss-codec";
      };
    };
    environment.etc."bluetooth/bt_stack.conf".source = "${floss}/share/floss/bt_stack.conf";
    environment.etc."bluetooth/interop_database.conf".source = "${interopDatabase}/interop_database.conf";
    services.dbus.enable = true;
    services.dbus.packages = [ policy ];
    security.polkit.enable = true;
    security.polkit.extraConfig = ''
      polkit.addRule(function(action, subject) {
        if (subject.user === "floss" && action.id === "org.freedesktop.systemd1.manage-units" &&
            action.lookup("unit") === "btadapterd@${toString cfg.floss.adapter}_${toString cfg.floss.adapter}.service" &&
            (action.lookup("verb") === "restart" || action.lookup("verb") === "stop")) {
          return polkit.Result.YES;
        }
      });
    '';
    boot.kernelModules = [ "bluetooth" "btusb" "uhid" ] ++ lib.optional cfg.floss.usbTransport.enable "hci_vhci";
    systemd.tmpfiles.rules = [
      "d /run/bluetooth 0750 floss bluetooth - -"
      "d /run/bluetooth/audio 0770 floss bluetooth-audio - -"
    ];
    systemd.services.floss-usb = mkIf cfg.floss.usbTransport.enable {
      description = "Floss userspace USB HCI and SCO transport";
      wantedBy = [ "multi-user.target" ];
      wants = [ "btmanagerd.service" ];
      before = [ "btmanagerd.service" ];
      after = [ "systemd-modules-load.service" "systemd-udev-trigger.service" ];
      serviceConfig = {
        Type = "notify";
        NotifyAccess = "main";
        ExecStart = "${packages.floss-usb}/bin/floss-usb";
        User = "floss-usb";
        Group = "floss-usb";
        RuntimeDirectory = "floss-usb";
        RuntimeDirectoryMode = "0750";
        UMask = "0027";
        Restart = "on-failure";
        RestartSec = 5;
        TimeoutStartSec = 30;
        TimeoutStopSec = 10;
        DevicePolicy = "closed";
        DeviceAllow = [ "/dev/vhci rw" "char-usb_device rw" ];
        CapabilityBoundingSet = [ "CAP_NET_ADMIN" "CAP_NET_RAW" ];
        AmbientCapabilities = [ "CAP_NET_ADMIN" "CAP_NET_RAW" ];
        RestrictAddressFamilies = [ "AF_UNIX" "AF_BLUETOOTH" "AF_NETLINK" ];
        NoNewPrivileges = true;
        ProtectSystem = "strict";
        ProtectHome = true;
        PrivateTmp = true;
        ProtectKernelTunables = true;
        ProtectKernelModules = true;
        ProtectKernelLogs = true;
        ProtectControlGroups = true;
        RestrictSUIDSGID = true;
        RestrictNamespaces = true;
        LockPersonality = true;
        MemoryDenyWriteExecute = true;
        SystemCallArchitectures = "native";
      };
    };
    systemd.services.btmanagerd = {
      description = "Floss Bluetooth manager";
      wantedBy = [ "multi-user.target" ];
      after = [ "dbus.service" "systemd-tmpfiles-setup.service" ] ++ lib.optional cfg.floss.usbTransport.enable "floss-usb.service";
      requires = lib.optional cfg.floss.usbTransport.enable "floss-usb.service";
      bindsTo = lib.optional cfg.floss.usbTransport.enable "floss-usb.service";
      wants = [ "dbus.service" ];
      path = [ pkgs.systemd ];
      preStart = ''
        ln -sfnT /etc/bluetooth/interop_database.conf /var/lib/bluetooth/interop_database.conf
        if [ ! -e /var/lib/bluetooth/btmanagerd.json ]; then
          printf '%s\n' '${builtins.toJSON {
            default_adapter = cfg.floss.adapter;
            "hci${toString cfg.floss.adapter}".enabled = cfg.powerOnBoot;
          }}' > /var/lib/bluetooth/btmanagerd.json
        fi
        printf '%s\n' floss > /var/lib/bluetooth/bluetooth-daemon.current
      '';
      serviceConfig = common // {
        Type = "dbus";
        BusName = "org.chromium.bluetooth.Manager";
        DeviceAllow = [ "/dev/rfkill rw" ];
        ExecStart = "${floss}/bin/btmanagerd --systemd --log-output=stderr";
      };
    };
    systemd.services."btadapterd@" = {
      description = "Floss Bluetooth adapter %i";
      after = [ "btmanagerd.service" "floss-codec.service" "systemd-tmpfiles-setup.service" ];
      requires = [ "btmanagerd.service" "floss-codec.service" ];
      # Requires alone does not follow an unexpected codec exit. Stop the
      # adapter too so it cannot keep feeding a dead AAC client; the manager's
      # PID watcher restarts enabled adapters after the codec becomes ready.
      bindsTo = [ "floss-codec.service" ];
      partOf = [ "btmanagerd.service" ];
      environment.FLOSS_HFP_SOFTWARE_HCI_TRANSPORT = if cfg.floss.softwareHciTransport then "true" else "false";
      serviceConfig = common // {
        Type = "dbus";
        BusName = "org.chromium.bluetooth";
        ExecStart = "${adapterLauncher} %i";
        # Upstream's Linux IoT device database uses relative filenames,
        # including its atomic .new and backup files. Keep all of them in
        # the private writable state directory under ProtectSystem=strict.
        WorkingDirectory = "/var/lib/bluetooth";
        # Native audio/timer threads request FIFO priority 1. Bound that
        # permission without granting CAP_SYS_NICE or unrestricted realtime.
        RestrictRealtime = false;
        LimitRTPRIO = 1;
        LimitRTTIME = 200000;
        DevicePolicy = "closed";
        DeviceAllow = [ "/dev/uhid rw" ];
        SupplementaryGroups = common.SupplementaryGroups ++ [ "floss-codec" ];
      };
    };
    systemd.services.floss-codec = {
      description = "Floss audio codec service";
      after = [ "dbus.service" ];
      requires = [ "dbus.service" ];
      serviceConfig = {
        Type = "dbus";
        BusName = "org.chromium.mmc.CodecManager";
        ExecStart = "${floss}/bin/mmc_service";
        User = "floss-codec";
        Group = "floss-codec";
        RuntimeDirectory = [ "mmc" "mmc/sockets" ];
        RuntimeDirectoryMode = "0750";
        UMask = "0007";
        NoNewPrivileges = true;
        ProtectSystem = "strict";
        ProtectHome = true;
        PrivateTmp = true;
        PrivateDevices = true;
        ProtectKernelTunables = true;
        ProtectKernelModules = true;
        ProtectKernelLogs = true;
        ProtectControlGroups = true;
        RestrictAddressFamilies = [ "AF_UNIX" ];
        CapabilityBoundingSet = "";
        RestrictSUIDSGID = true;
        RestrictNamespaces = true;
        LockPersonality = true;
        SystemCallArchitectures = "native";
        MemoryDenyWriteExecute = true;
        SystemCallFilter = [ "@system-service" ];
        ProtectProc = "invisible";
        LimitRTPRIO = 1;
        LimitRTTIME = 200000;
        TasksMax = 64;
        Restart = "on-failure";
        RestartSec = 2;
        TimeoutStopSec = 15;
      };
    };
    systemd.user.services.floss-pairing = {
      description = "Floss Bluetooth pairing prompts";
      wantedBy = [ "graphical-session.target" ];
      after = [ "graphical-session-pre.target" ];
      partOf = [ "graphical-session.target" ];
      unitConfig.ConditionGroup = "bluetooth";
      environment.BLUEDEVIL_FLOSS_ADAPTER = toString cfg.floss.adapter;
      serviceConfig = {
        ExecStart = "${packages.bluedevil}/bin/bluedevil-floss-pairing";
        Restart = "on-failure";
        RestartSec = 3;
        TimeoutStopSec = 15;
        NoNewPrivileges = true;
        RestrictSUIDSGID = true;
        RestrictAddressFamilies = [ "AF_UNIX" ];
        LockPersonality = true;
        UMask = "0077";
      };
    };
    systemd.user.services.floss-policy = {
      description = "Floss paired-device reconnect policy";
      wantedBy = [ "graphical-session.target" ];
      after = [ "graphical-session-pre.target" ];
      partOf = [ "graphical-session.target" ];
      unitConfig.ConditionGroup = "bluetooth";
      environment.BLUEDEVIL_FLOSS_ADAPTER = toString cfg.floss.adapter;
      serviceConfig = {
        ExecStart = "${packages.bluedevil}/bin/bluedevil-floss-policy";
        Restart = "on-failure";
        RestartSec = 3;
        TimeoutStopSec = 15;
        NoNewPrivileges = true;
        RestrictSUIDSGID = true;
        RestrictAddressFamilies = [ "AF_UNIX" ];
        LockPersonality = true;
        UMask = "0077";
      };
    };
    systemd.user.services.floss-audio = mkIf config.services.pipewire.enable {
      description = "Automatic Floss desktop audio";
      wantedBy = [ "graphical-session.target" "pipewire.service" ];
      after = [ "pipewire.service" "wireplumber.service" ];
      requires = [ "pipewire.service" ];
      wants = [ "wireplumber.service" ];
      partOf = [ "graphical-session.target" "pipewire.service" ];
      unitConfig.ConditionGroup = "bluetooth-audio";
      serviceConfig = {
        ExecStart = "${packages.pw-floss}/bin/pw-floss --auto --adapter ${toString cfg.floss.adapter}";
        Restart = "on-failure";
        RestartSec = 3;
        TimeoutStopSec = 15;
        NoNewPrivileges = true;
        RestrictSUIDSGID = true;
        RestrictAddressFamilies = [ "AF_UNIX" ];
        LockPersonality = true;
        UMask = "0077";
      };
    };
    services.udev.extraRules = ''
      KERNEL=="uhid", GROUP="floss", MODE="0660"
      KERNEL=="rfkill", GROUP="floss", MODE="0660"
    '' + lib.optionalString cfg.floss.usbTransport.enable ''
      KERNEL=="vhci", GROUP="floss-usb", MODE="0660"
      SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_device", ATTR{idVendor}=="0bda", ATTR{idProduct}=="c123", GROUP="floss-usb", MODE="0660"
    '';
    services.pipewire.package = packages.pipewire;
    services.pipewire.extraConfig.pipewire-pulse."90-floss-passive-meters" =
      builtins.fromJSON (builtins.readFile ./passive-meters.json);
    services.pipewire.wireplumber.package = packages.wireplumber;
    environment.systemPackages = [ floss packages.pw-floss packages.bluedevil ];
    environment.sessionVariables.BLUEDEVIL_FLOSS_ADAPTER = toString cfg.floss.adapter;
  };
}
