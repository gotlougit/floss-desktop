# Floss desktop for NixOS

Floss is the codename for the Linux path inside of Android's [Bluetooth stack](https://chromeos.dev/en/posts/androids-bluetooth-stack-fluoride-comes-to-chromeos)
that was built as part of unifying ChromeOS with Android. As a side effect,
it was theoretically possible to use the Android implementation of Bluetooth
in place of BlueZ, the traditional Bluetooth daemon used on Linux.

I spent some time and came up with this: a NixOS module that can set up forked
Wireplumber and Bluedevil (the Bluetooth interface code for KDE Plasma) and allow
using Floss instead of BlueZ. I have been dogfooding it and will keep updating the
patches accordingly. For now my main usage is with Bluetooth audio devices.

## Architecture

The flake assembles a patched Floss daemon, BlueDevil and WirePlumber, an
upstream PipeWire build with BlueZ disabled, and the standalone `pw-floss`
bridge. At the system level, `btmanagerd` and `btadapterd` own the Bluetooth
controller and expose Floss over D-Bus; a separate codec service handles AAC,
aptX, aptX HD and LDAC.

The optional Rust `floss-usb` service supplies USB SCO transport through the
stock virtual-HCI driver for one Realtek 0bda:c123 adapter. It requires no kernel
patch. Its code and mock tests are implemented; physical headset calls remain
unverified. See [the transport guide](floss-usb/README.md).

In each desktop session, the patched BlueDevil provides the Plasma UI, pairing
prompts and reconnect policy. `pw-floss` controls Floss over D-Bus, transfers
PCM through Unix sockets, and publishes normal PipeWire sink/source nodes for
WirePlumber to route. Floss therefore remains responsible for Bluetooth
profiles, codecs and transport, while PipeWire handles desktop audio mixing and
resampling. The NixOS module supplies the service accounts, permissions, D-Bus
policy, systemd units and package substitutions that connect these pieces.

The bridge exposes a standard audio card with Automatic, SBC, AAC, aptX,
aptX HD, LDAC and Hands-free choices in KDE sound settings, limited to the
connected device's capabilities. Automatic mode switches for real microphone use; passive panel
meters leave the transport alone. See [the bridge protocol](pw-floss/PROTOCOL.md)
for profile behavior and validation limits.

## LLM usage disclosure

Almost the entire codebase is LLM owned. As the foundations are solid enough,
and the alterations required involve swapping out BlueZ for Floss, I believe
it is a decent enough start to get a more secure Bluetooth stack on Linux with
little effort.

## Build

```sh
nix flake check --no-build
nix build .#stack
```

The stack currently targets x86_64 Linux. Builds and isolated mock checks pass,
but physical controller/headset operation and production service confinement
remain unverified. Trust/block controls, file transfer, active-seat arbitration,
general `org.bluez` compatibility and multi-device audio are not implemented.

See [NIXOS.md](NIXOS.md) for installation, operation and rollback guidance.

## Source updates

[`patches/sources.json`](patches/sources.json) is the sole source manifest. For
each component it records the official upstream URL, pinned revision, patch file,
patch size and patch digest. Bluetooth service and codec changes are split into
additional patches because the Nix build consumes those source sets separately.

To update a component:

1. Fetch the official upstream repository and check out the recorded revision.
2. Apply the component's patches in manifest order.
3. Rebase or otherwise adapt the changes to the new upstream revision.
4. Export the resulting diff, then update its size and digest in the manifest.
5. Update the matching Nix source hash and run the build and checks above.

`tools/export-patches.py` exports patches from sibling source checkouts whose
HEADs match the revisions recorded in the manifest.

`pw-floss` is not a patch or a fork: update and test it directly under
`pw-floss/`.

## License

Original material in this repository is licensed under GPL-2.0-only; see
[LICENSE](LICENSE). The extracted `pw-floss` project is MIT-licensed, matching
its PipeWire origin. Patches and packaged source retain the copyright and
license terms of their respective upstream projects.
