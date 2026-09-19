# Floss desktop for NixOS

Experimental Floss replacement for the BlueZ daemon with Plasma, PipeWire and
WirePlumber integration.

This repository is the complete, reproducible source for the integration. It
fetches pinned revisions from the official Bluetooth, BlueDevil and WirePlumber
upstream projects and applies the patches stored in `patches/`. PipeWire comes
unmodified from the pinned nixpkgs with BlueZ disabled; the standalone C
[`pw-floss`](pw-floss/) client is maintained and built directly here. No
separately maintained source repository or local checkout is a build input.

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
