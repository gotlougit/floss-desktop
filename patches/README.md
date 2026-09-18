# Portable Floss desktop patches

`sources.json` records the upstream Git URL and base commit for each fork. Each
patch set contains all current changes against that commit, including new
files. Apply the primary `patch` first, then any `extraPatches` in listed order.
The Bluetooth service runtime patch contains the Linux Rust daemon/client changes;
the native archive needs only the primary patch, so service-only changes reuse
the compiled native archive. The codec-service patch contains MMC daemon and
encoder-server changes and is applied only by the separate codec derivation.
The native daemon uses the MMC client and does not link FFmpeg into its process.
`tools/export-patches.py` keeps these file sets
separate using the manifest's `files` lists (a trailing slash includes a directory
recursively). Native CXX bridge declarations remain in the primary patch.
Use these files when packaging remotely fetched upstream sources; the
patches do not require a path to any local reference checkout.

Apply the matching patch at the root of its pinned upstream source tree with
`patch -p1` or `git apply`. Applying a patch to another version may require a
rebase. The Nix builds apply these patches to the pinned remote sources. See
[BUILD-STATUS.md](../BUILD-STATUS.md) for completed builds and remaining work;
isolated runtime checks are recorded under `runtime-results/`.

`patchFileHash` is the SHA-256 SRI digest of the raw patch file. It is **not** a
hash of remotely fetched source, a Nix `fetchpatch` normalized-output hash, or
evidence of a successful build. Remote source hashes are pinned separately in
`../nix/`. The standalone flake and NixOS module are described in
[NIXOS.md](../NIXOS.md).

These snapshots include substantial deletions of BlueZ-only implementation,
dependencies, and build configuration. They retain the Floss integration and
ordinary desktop/audio functionality described in each repository's README.
The Bluetooth patch adds caller-owned audio sessions, A2DP position feedback,
and the HFP PCM API and transport changes used by the PipeWire bridge.
The exports include the source fixes described in [FLOSS-FIXES.md](../FLOSS-FIXES.md). BluezQt remains unchanged and is not required by the
revised BlueDevil fork.

No Git staging, commits, or source downloads were needed to export the patches.
