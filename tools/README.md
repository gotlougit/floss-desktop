# Development tools

These tools update the portable patch set and exercise built components without
activating host Bluetooth or audio services. Run Python entry points with
`--help` for their required package, compiler and output arguments.

## Patch maintenance

- `export-patches.py` exports component diffs from sibling upstream checkouts at
  the revisions in `patches/sources.json` and refreshes patch metadata.
- `nix-snapshot.py` copies only the distributable flake inputs to a clean build
  directory.

## Isolated integration checks

- `smoke-aac-isolated.py` exercises the real MMC AAC encoder over a private
  D-Bus and socket filesystem.
- `smoke-audio-isolated.py` starts private PipeWire and WirePlumber instances;
  pass the standalone bridge executable with `--bridge`.
- `smoke-bridge-isolated.py` tests actual A2DP/HFP PCM transport against a mock
  peer; `smoke-auto-bridge-isolated.py` runs the automatic device/profile matrix.
  Both take PipeWire and `pw-floss` as separate inputs.
- `smoke-hci-isolated.py` starts the real adapter daemon against the scripted
  private HCI peer in `mock-hci-controller.py` and `mock-hci-socket.c`.
- `smoke-kde-isolated.py` loads the built BlueDevil UI offscreen with private
  buses and the fixtures under `kde-mock/`.
- `floss-runtime-smoke.sh` checks the packaged daemons on a private bus.

## Focused checks

- `test-floss-callback-registry.py` tests the production Rust callback registry
  in an offline harness.
- `test-aac-feeder.py`, `test-audioconvert-disconnect.py`, and
  `floss-properties-smoke.sh` exercise specific production control paths.
- The rfkill unit tests live in the patched Bluetooth source and operate only on
  temporary sysfs-shaped fixtures.

These checks cover controlled process, IPC and codec behavior. They do not prove
physical controller, radio, headset, kernel transport or production sandbox
behavior.
