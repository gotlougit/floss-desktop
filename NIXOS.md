# Building and using Floss desktop on NixOS

This flake fetches pinned upstream sources and applies the patches shipped beside
it. No separate Bluetooth, PipeWire, BlueDevil, BluezQt or WirePlumber checkout
is a build input. Copy `flake.nix`, `flake.lock`, `nix/` and `patches/` together
to use it elsewhere. The supported build platform is currently x86_64-linux.
Keep the flake's pinned Nixpkgs input: the builds use its matching Qt, KDE and
PipeWire dependencies. Replacing that pin is a separate compatibility change.

## Build without enabling anything

From a directory containing just those distributable files:

```sh
nix build .#stack
nix flake check --no-build
```

The `result/` directory contains links named `floss`, `pipewire`, `wireplumber`,
and `bluedevil`. Individual targets such as `nix build .#floss` are available.
Builds disable test execution and do not start Bluetooth or audio services.

## Integrate into your NixOS configuration

Put the distributable files in a persistent directory, not `/tmp`, and add that
directory as an input to your system flake. For example:

```nix
{
  # Match the versions used for these builds.
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/dc5d91f840324650bac8c379428c7037a416959a";
  inputs.flossDesktop.url = "path:/home/YOU/Code/floss-desktop-flake";

  outputs = { nixpkgs, flossDesktop, ... }: {
    nixosConfigurations.HOSTNAME = nixpkgs.lib.nixosSystem {
      system = "x86_64-linux";
      modules = [
        ./configuration.nix
        flossDesktop.nixosModules.default
        {
          hardware.bluetooth.enable = true;
          hardware.bluetooth.floss.users = [ "YOUR_LOGIN" ];
          services.pipewire.enable = true;
          services.pipewire.pulse.enable = true;
          services.pipewire.wireplumber.enable = true;
        }
      ];
    };
  };
}
```

Merge this into your existing flake; retain its existing host modules, hardware
configuration and other inputs. A compatible Plasma/KDE version is needed;
these builds use the Nixpkgs revision pinned in `flake.lock`. For the first
system build, using the same Nixpkgs revision avoids mixing KDE releases.

Importing the module replaces NixOS's stock Bluetooth module, so the existing
`hardware.bluetooth.enable = true` enables Floss. Remove BlueZ-specific
`hardware.bluetooth.settings`, plugin, input/network and package overrides.
The module installs the patched Floss BlueDevil package and selects the patched PipeWire
and WirePlumber packages. It substitutes the stock BlueDevil desktop package
and omits the old BlueZ/OBEX tools from the system package list. Unrelated
applications may still have their own BlueZ dependencies or API requirements;
this stack does not implement `org.bluez` compatibility.

The module grants only listed users Bluetooth control and PCM socket access.
Those permissions persist across seat changes; there is no automatic active-seat
arbitration yet. Log out and back in after adding the groups. The module enables
a dedicated `floss` account, restricted D-Bus policy, and sandboxed manager and
adapter units with only the Bluetooth network capabilities and required writable
runtime/state/log directories. Bond state and daemon logs are private to the
`floss` account; desktop group access is limited to control and audio sockets. Adapter threads may request only FIFO priority 1,
with a bounded realtime CPU limit. The manager can control only the configured
adapter unit through a narrow polkit rule. This sandbox has been evaluated as
configuration, not verified against hardware. The daemon/codec restrictions
also follow the cached BlueZ service baseline: `MemoryDenyWriteExecute`,
`ProtectProc=invisible`, and the `@system-service` syscall group. Floss retains
its own required HCI capabilities, address families, UHID access and bounded
realtime scheduling; copying BlueZ settings is not a runtime test.

The module installs the pinned upstream interoperability database and links it
at the static path expected by Floss. This retains device-specific codec
exceptions. The codec service runs under its own account; unexpected codec loss
also stops the adapter so Floss's manager can recover it instead of leaving a
dead AAC client active. That recovery path has been source-reviewed, not tested
under an activated systemd deployment.

Build your system configuration before deciding to activate it:

```sh
nixos-rebuild build --flake /path/to/your/system-flake#HOSTNAME
```

This compiles a system closure without switching services. Once you choose to
activate it, run `nixos-rebuild switch --flake ...#HOSTNAME` as root using your
normal administration method. **The work in this repository does not perform
that activation for you.** Keep the prior NixOS generation for rollback.
Bluetooth pairing state is stored under `/var/lib/bluetooth`; preserve that
state before changing stacks. BlueZ bonds are not automatically migrated.

## Automatic audio

After activation and a new login, pair and connect using Plasma's existing
Bluetooth applet/settings. The module starts a per-user `floss-audio.service`
for users in `hardware.bluetooth.floss.users`. It discovers connected devices,
uses Floss's authoritative PCM format, and lets WirePlumber route ordinary audio
streams. No terminal bridge launch or sample-rate configuration is needed.
Normal saved output/input preferences still apply in Plasma's audio controls.

Floss owns SBC/AAC encoding. AAC runs in a dedicated, sandboxed
`floss-codec.service` using the upstream MMC codec service and FFmpeg encoder.
The upstream AAC source advertises 44.1 kHz stereo; PipeWire resamples application
PCM to that negotiated format. AAC requires support at the remote device.
SBC mono and stereo feeder formats are both supported automatically.

An application recording from the headset microphone triggers HFP automatically;
two seconds after recording stops, the bridge returns to A2DP. Sink/source node
identities survive that transition. Built-in microphone recording and passive
level meters do not trigger HFP. A playback-only speaker gets no microphone.
Failed HFP activation restores A2DP with a retry delay. HFP uses CVSD/mSBC rather
than AAC; classic Bluetooth does not maintain AAC stereo during a headset call.

The initial deployment supports adapter `hci0` and one active audio device.
Additional connected devices do not steal the active device. Disconnects and
daemon restarts trigger rediscovery. The separate `floss-policy.service` also
initiates paired-device reconnection in the background, with bounded exponential
backoff. Explicit Disconnect/Forget persists inhibition; Connect/Pair enables
retries again. Policy state is shared in the user's configuration directory as
`bluedevil-floss-reconnect.ini`. Only one worker per session/adapter owns retries.
Individual profile loss while the ACL link survives remains outside this policy.
Battery readings and confirmed device renaming are exposed in tray/settings.
Monitor the supplied services if necessary:

```sh
systemctl status btmanagerd.service btadapterd@0_0.service floss-codec.service
systemctl --user status floss-audio.service floss-policy.service floss-pairing.service
journalctl --user -b -u floss-audio.service -u floss-policy.service -u floss-pairing.service
```

Do not start another `pw-floss` process while the user service owns the transport.
Manual `--profile`/`--device` modes remain diagnostic tools, not installation steps.

## Current boundaries

Controller access, pairing, over-air codec negotiation, timing quality and long
calls still require physical-device testing. HFP requires working SCO transport
under Floss's userspace HCI ownership. The optional
`hardware.bluetooth.floss.softwareHciTransport` setting skips Chromium-specific
management commands; it does not supply missing USB isochronous support. Leave
it disabled unless that path is known to work on your controller.

There is no active-seat arbitration, simultaneous independent headset output,
LE Audio/LC3 voice, OBEX transfer or general `org.bluez` compatibility. Do not run
BlueZ and Floss against the same controller. Isolated tests do not establish
physical controller, radio, headset or production-system behavior.

The manager alone receives `/dev/rfkill` access in its device sandbox; the udev
rule grants the private `floss` group access. Desktop processes use the manager's
D-Bus API and receive no new rfkill group membership. Requests select a mapped
Bluetooth hciN switch, reject hardware-unblock attempts, and never use a global
radio operation. The UI confirms software-unblock state before requesting Start.
Unknown or platform-wide mappings remain unavailable.

`floss-pairing.service` runs the standalone pairing window during the graphical
session, hidden while idle. Its existing responder election cooperates with the
tray and settings. This does not add multi-session/active-seat arbitration.
