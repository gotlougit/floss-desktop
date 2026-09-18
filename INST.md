# Install the Floss desktop fork on your NixOS configuration

These instructions target `gotlougit/nix-config`, host **kratos**, user **gotlou**.
The inspected GitHub revision is `3705f79f41e3803a54772e4c54725e209f356463`.
The clone in this workspace has the proposed edits. A standalone review patch
is provided as `nixos-floss.patch`; apply it to your configuration repository
only after reviewing it. No system activation has been performed.

All four packages build from pinned remote sources plus the included patches.
They do not use the Bluetooth, PipeWire, BlueDevil or WirePlumber checkouts.
See `BUILD-STATUS.md` for build results and `RUNTIME-STATUS.md` for the limits of
the unprivileged runtime checks. A successful build or private-session smoke
check does not establish working pairing, A2DP playback or headset microphone
transport on your controller.

## 1. Review and apply the configuration patch

From your configuration repository, with a clean working tree:

```sh
git apply --stat /path/to/nixos-floss.patch
git apply --check /path/to/nixos-floss.patch
git apply /path/to/nixos-floss.patch
git diff
```

The patch targets the inspected `gotlougit/nix-config` revision above. If your
local configuration has diverged, review and apply the corresponding edits
manually instead of forcing the patch. It updates only the Floss input, module,
Bluetooth/audio configuration and relevant lockfile entries.

## 2. Published input and module

The integration repository is `https://github.com/gotlougit/floss-desktop`.
The patch pins `inputs.flossDesktop.url` to a specific commit and imports
`inputs.flossDesktop.nixosModules.default` in `nixosConfigurations.kratos.modules`.
No workspace checkout or vendored files are required.

The four modified source repositories are linked in that repository's README.
The Nix packages retain the validated upstream revisions plus portable patches;
they do not follow moving branches in the source forks. This preserves the
already-built derivations. Changing a fork alone does not update these packages:
export the corresponding patch and update its manifest before publishing a new
integration revision.

The configuration patch also pins the host's Nixpkgs to
`dc5d91f840324650bac8c379428c7037a416959a`, matching the built KDE/Qt packages.
The Floss input retains its own matching pin. Other inputs are not upgraded.
Keep the existing Home Manager, Plasma Manager, host and system modules.
The Floss module disables the stock Bluetooth module and makes
`hardware.bluetooth.enable` select Floss.

## 3. Replace your Bluetooth module

Replace the entire contents of `system/bluetooth.nix` with:

```nix
{ ... }:
{
  hardware.bluetooth.enable = true;
  hardware.bluetooth.floss.users = [ "gotlou" ];
  hardware.bluetooth.powerOnBoot = true;
}
```

In particular, remove the existing `systemd.services.bluetooth.serviceConfig`
block. Leaving it behind creates a stray BlueZ-named service definition after
the stock module is disabled. The supplied module defines its own sandboxed
`btmanagerd` and `btadapterd@` units. It already incorporates compatible
restrictions from the pinned BlueZ service; retain the supplied Floss settings.

The module also installs Floss's pinned device-interoperability database,
including AAC exceptions, and manages its static link under `/var/lib/bluetooth`.

`powerOnBoot` seeds the initial manager configuration. Once Floss has saved an
enabled/disabled state, that state takes precedence.

## 4. Remove the old BlueZ audio configuration

In `hosts/kratos/services/sound.nix`, keep your existing PipeWire settings,
including ALSA, 32-bit ALSA, PulseAudio compatibility, JACK, WirePlumber and
`security.rtkit.enable`.

NixOS supplies 32-bit ALSA compatibility from its stock 32-bit PipeWire package;
the native daemon and bridge use this fork. That compatibility package can retain
transitive BlueZ libraries, but does not start another Bluetooth daemon. If you
do not need 32-bit audio clients, disable `alsa.support32Bit`.

Delete this entire declaration:

```nix
environment.etc."wireplumber/wireplumber.conf.d/50-bluetooth-config.conf".text = ''
  monitor.bluez.properties = {
    bluez5.enable-sbc-xq = true
    bluez5.enable-msbc = true
    bluez5.enable-hw-volume = true
    bluez5.headset-roles = [ hsp_hs hsp_ag hfp_hf hfp_ag ]
  }
'';
```

The Floss module selects the patched PipeWire and WirePlumber packages and
substitutes the patched BlueDevil for Plasma's stock package. Your panel's
`org.kde.plasma.bluetooth` applet ID remains the same, so no panel rewrite is
needed. Floss does not provide an `org.bluez` compatibility API for other apps.

## 5. Persistence and rollback

`hosts/kratos/impermanence.nix` already persists `/var/lib/bluetooth` under
`/persist/system`; retain it. Do not persist `/run/bluetooth` or `/run/mmc`,
which hold session sockets and transient data.

Preserve a backup of the existing Bluetooth state before the first activation.
Floss uses this directory and changes its ownership to its dedicated account;
this module does not migrate BlueZ bonds. Expect to pair devices again. Keep a
previous NixOS generation and a wired input device available for rollback.
If you roll back, log in again to replace the desktop/audio processes too.

## 6. Evaluate, build, then activate when ready

The supplied patch includes lockfile entries. Review the diff before building:

```sh
cd /home/gotlou/nixos
git diff -- flake.nix flake.lock system/bluetooth.nix hosts/kratos/services/sound.nix
nix flake check --no-build .
nixos-rebuild build --flake .#kratos
```

No new configuration files need staging for this patch: all four changed files
are already tracked. Do not run an unrestricted `nix flake update` as part of
installation.

Earlier full-host evaluation stopped at an unavailable Stylix/base16 derivation.
Standalone module evaluation and a representative Plasma/audio configuration
passed; those checks do not establish that every unrelated host service builds.
See `PUBLISHING.md` for the checks performed on the published configuration patch.

None of the commands above activates services. When ready, use your normal
administrator workflow to run:

```sh
nixos-rebuild switch --flake /home/gotlou/nixos#kratos
```

Switching needs administrator privileges; the checks performed for this task do
not execute it or use sudo. Log out and back in after activation, or reboot, so
new group membership, the patched Plasma plugin and audio processes take effect.

## 7. Pair and use audio

After activation and a new login, use Plasma's Bluetooth applet/settings to
pair and connect the device. The module starts `floss-audio.service` in your
user session automatically. There is no bridge command, device address, PCM
rate or manual profile switch to configure. The session also runs
`floss-pairing.service`: its pairing window remains hidden until needed, so
prompts do not depend on having the tray or settings loaded. The existing
responder election prevents multiple views from answering the same prompt.

The Bluetooth enable switch now handles a software rfkill block for the selected
controller: it unblocks, verifies the new state, then requests adapter startup.
A hardware block instead shows a message directing you to the hardware switch.
The manager targets only a mapped kernel hciN Bluetooth switch; platform-wide
radio switches and unknown controller mappings cannot be safely changed through
this interface. No global unblock operation is used.

The bridge discovers connected audio devices and publishes normal PipeWire
output/input nodes. WirePlumber applies its normal routing and saved device
preferences. If you previously selected another output or microphone, select
the headset in Plasma's usual audio controls; application routing preferences
still apply. Speakers without HFP support do not get a fictitious microphone.

Stereo playback uses Floss's negotiated SBC or AAC codec. AAC encoding runs in
Floss's separate `mmc_service`; PipeWire supplies PCM and resamples to the actual
format reported by Floss. This upstream AAC source advertises 44.1 kHz stereo;
your applications can continue producing 48 kHz audio. AAC is negotiated only
when the remote device supports it, with SBC available as a fallback.
SBC devices can use either mono or stereo PCM; the bridge adapts automatically.

When an application records from the headset microphone, the bridge switches to
HFP automatically, retaining its PipeWire node identities. It returns to stereo
playback two seconds after recording stops. Recording from a built-in microphone
or observing levels with a passive meter does not request HFP. HFP uses CVSD or
mSBC, not AAC, so headset microphone use reduces playback quality as usual for
classic Bluetooth. Failed HFP activation restores stereo playback and backs off
before another attempt.

The first deployment supports **hci0 and one active audio device**. A second
connected device does not steal the selected device. The bridge rediscovers
available devices after disconnection or daemon restart.

The session also starts `floss-policy.service`, which reconnects previously paired
devices without needing an open Bluetooth window. It retries with increasing
delays (up to five minutes), waits for observed connection state, and preserves
backoff across restarts. Disconnect or Forget disables retries for that device;
Connect or Pair enables them again. Preferences are shared by the tray, settings
and background worker in `$XDG_CONFIG_HOME/bluedevil-floss-reconnect.ini`
(normally `~/.config`). Persist this file with your home configuration if desired.
A brief reconciliation window disconnects late completions of an earlier request.
This is reconnect policy, not a security block on incoming connections. Recovery
of an individual profile while the underlying Bluetooth link stays connected
is still separate work.

Device details now show available battery readings, including separate components
when the headset supplies them. Unknown battery levels are omitted. Rename is
available in both the tray and settings; leaving the name empty restores the
original device name. A rename is only confirmed after Floss reports the new value.

For diagnostics only:

```sh
systemctl status btmanagerd.service btadapterd@0_0.service floss-codec.service
systemctl --user status floss-audio.service floss-policy.service floss-pairing.service
journalctl -b -u btmanagerd.service -u btadapterd@0_0.service -u floss-codec.service
journalctl --user -b -u floss-audio.service -u floss-policy.service -u floss-pairing.service
```

Do not launch a second bridge alongside the automatic service. Its singleton
reservation deliberately prevents competing access to Floss's PCM transport.

HFP still requires a Linux controller/kernel transport that carries SCO under
Floss's HCI ownership. Mock tests do not establish that on your hardware.
`hardware.bluetooth.floss.softwareHciTransport = true` bypasses Chromium-specific
management commands; it does not implement missing USB isochronous support and
is not a general-purpose fix. Leave it disabled unless that transport has been
verified for your controller. Permissions are granted by group membership,
not active-seat arbitration. Do not run BlueZ against the same controller.
