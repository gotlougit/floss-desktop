# Floss desktop for NixOS

Experimental Floss replacement for the BlueZ daemon and Plasma/PipeWire integration.
Start with [INST.md](INST.md). Builds and isolated mock checks pass; physical
controller/headset operation and production service confinement remain unverified.
Trust/block controls and file transfer are not implemented.

## Source repositories

All modified code is published on the `floss` branch:

- [Bluetooth / Floss](https://github.com/gotlougit/floss-bluetooth/tree/floss)
- [PipeWire](https://github.com/gotlougit/floss-pipewire/tree/floss)
- [BlueDevil](https://github.com/gotlougit/floss-bluedevil/tree/floss)
- [WirePlumber](https://github.com/gotlougit/floss-wireplumber/tree/floss)

`published-projects.json` records exact fork commits and their upstream bases.
BluezQt is unchanged and does not need a fork. Upstream copyright and license
notices remain in their respective projects.

## Reproducible packaging

This flake builds pinned remote upstream sources with the portable changes in
`patches/`. It does not reference local checkouts or moving fork branches.
The fork commits contain those same changes. This packaging preserves the
previously validated build inputs and does not require downloading Git histories.

```sh
nix flake check --no-build
nix build .#stack
```

Import `nixosModules.default`, enable `hardware.bluetooth.enable` and set
`hardware.bluetooth.floss.users` to the intended local users. See INST.md for the
complete configuration, persistence and rollback instructions.

To update code, develop in the matching source fork, export the diff against the
upstream revision in `patches/sources.json`, and refresh the patch hash and size.
Bluetooth changes are split between native, Rust service and codec patches.
`tools/export-patches.py` automates this with sibling checkouts at the recorded
upstream bases. Source forks and the packaging repository must be updated together.

See [BUILD-STATUS.md](BUILD-STATUS.md) and [RUNTIME-STATUS.md](RUNTIME-STATUS.md)
for validation scope and known limitations.

See [September 19 fixes](FIXES-2026-09-19.md) for KDE output visibility, AAC short-read framing, state-file persistence, and upgrading an existing installation.
