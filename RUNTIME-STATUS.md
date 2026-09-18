# Unprivileged runtime checks

Checks use private D-Bus/audio sessions and temporary state. They do not activate
a NixOS generation, restart host services, pair devices, or take over `hci0`.
No sudo is used.

## AAC and automatic audio

The final packaged PipeWire and WirePlumber pass **11 automatic device cases**:
SBC and AAC-advertised headsets, stereo and mono speakers, CVSD, mSBC, voice-only
headsets, delayed transport release, HFP denial, device/daemon lifecycle and
saved/default routing. Tests use actual playback/recording clients and verify:

- Automatic A2DP/HFP changes retain sink/source node identities and carry
  nonzero playback and microphone PCM with the expected tone.
- A normal stereo application plays correctly through a mono SBC feeder.
- Untargeted clients use the headset; explicitly choosing the built-in
  microphone is respected across reconnects.
- Passive meters and unrelated microphones do not trigger HFP.
- Disconnects during playback/calls, daemon replacement, and capability changes
  recover; a second connected device does not steal active audio.
- HFP refusal restores A2DP and respects retry backoff.

The seven manual-session transport/error cases and eight CLI/policy checks also
pass on those same packages. Reports in
`runtime-results/{auto-bridge,bridge,audio}/summary.json` record executable paths
and hashes. The bridge is
`/nix/store/2v9hcni978hrmhrr4qi1iyvvrjcy51za-pipewire-floss-1.7.0-floss-744a6b547ddb/bin/pw-floss`,
SHA-256 `6c82476e893117a9ac53cbd423a856cb96d93d41bb46f5fd0ccd2f50f36519ee`.

AAC-advertised mock devices are not AAC encoder tests. The separate real Floss
MMC encoder check **passed** on the final codec package
`/nix/store/69954hxw6hlf99rx3fb54kd5b1n51av9-floss-codec-0.1-745ee92b87ed`.
At both 44.1 and 48 kHz, 30 encoded AAC frames decoded into 61,440 PCM samples;
more than 99.8% of measured energy matched the submitted 440 Hz tone. Invalid
configuration and truncated PCM were rejected, and 12 encoder session turnovers
passed. The Linux source still advertises 44.1 kHz AAC; this test does not change
that advertisement or exercise over-air negotiation. Results are in
`runtime-results/aac/summary.json`; see `tools/ISOLATED-AAC-SMOKE.md`.

The complete clean Nix build passes, including native AAC/SBC hooks and all
three Rust executables. The final daemon passes scripted-controller startup,
new API checks, disconnected-device rejection, and SIGTERM shutdown (exit 0 in
1.368 seconds). All three executables also pass `--help`; the no-controller
adapter exits cleanly. The final native archive's PCM hooks were verified by
symbol inspection, recorded in `runtime-results/aac/pcm-hook-symbols.txt`.

The codec service and automatic user unit pass module evaluation. The adapter
binds to the codec service so unexpected codec loss stops the adapter; manager
PID monitoring can recover enabled adapters. This failure path was reviewed
against source and local systemd documentation, not tested through host service
activation. Hard-kill recovery can take the manager's 60-second PID polling
interval. The module now also supplies the upstream static interoperability
rules rather than silently omitting device-specific codec exceptions.

## Desktop pairing, reconnect and radio controls

The current BlueDevil package passes **13 isolated desktop scenarios** using
private buses and offscreen Qt: backend, KCM, applet, device features, reconnect,
headless reconnect policy, background pairing, pairing responder handoff, and
five rfkill cases. These cover callback spoof rejection, one confirmation per
pairing request, battery/alias state, persistent reconnect inhibition/backoff,
hard-block refusal, confirmed software unblock, unavailable mapping, API failure,
and a software unblock that is accepted but never takes effect.

`runtime-results/kde/summary.json` combines the original six passing scenarios
and seven additional passing scenarios on the same final package; it records
that provenance. Failed fixture runs are retained separately.

Six isolated Rust rfkill tests pass using synthetic sysfs and an injected writer
(`runtime-results/rfkill/summary.json`). The actual manager API rejects unknown
controller mappings in a private namespace. These checks never write to the
host's rfkill device. They do not establish physical switch behavior.

All six generated system/user units pass static systemd verification, including
the background reconnect and pairing services. Production service activation
and hardware access under these restrictions remain untested.

## SocketManager client isolation

The current Floss package passes a private-bus test with two independent clients
against the actual adapter daemon. All **16 callback-ID methods** reject a
foreign owner with `AccessDenied`. Same-path registrations remain separate
between clients and idempotent for one owner. Valid-owner missing-socket calls,
unregister, replacement registration, client reconnection and forged disconnect
rejection pass. The report is in `runtime-results/hci/summary.json`.

The previous package fails the same-path isolation check, preserved as a
negative control in `runtime-results/socket-authorization-before/summary.json`.
Two focused production-registry tests and compilation of the exporter macro
also pass (`runtime-results/socket-registry/summary.json`).

The fix authenticates the bus sender under the same manager lock as dispatch
and scopes callback identities by owner plus path. It prevents another allowed
D-Bus client from operating on a victim's callback ID; direct file-descriptor
theft was not established. No real RFCOMM/L2CAP connection or listener is created
by this test. Cleanup of live native sockets remains source-reviewed, not
runtime-proven. This is not per-device Trust/Block or OBEX support.

## Passed checks

- Built PipeWire starts in a private runtime directory.
- Built WirePlumber starts with hardware monitors disabled and connects to it.
- Synthetic Floss-labelled sink/source nodes are recognized and selected by
  WirePlumber policy. This is not an actual Bluetooth or PCM-streaming check.
- `pw-floss` CLI validation rejects invalid profile/rate combinations and fails
  explicitly when the Floss daemon is absent on the private bus.
- The actual `pw-floss` binary passes seven scenarios against a private mock
  Floss media service: ten-second HFP duplex PCM with the expected 500 Hz capture
  tone, A2DP playback/position feedback, invalid format, configuration-generation
  race, PCM disconnect, daemon-owner loss, and a regressing A2DP position counter.
  Duplicate bridge ownership is rejected. Leases are released and nodes removed
  after each scenario. This exercises the bridge, not actual Floss codecs.
- The BlueDevil backend initializes on a private bus with no adapters, and its
  Bluetooth KCM loads offscreen with matching Qt/KDE imports.
- The actual Bluetooth applet loads in `plasmawindowed` after removing an action
  property unavailable in pinned Plasma 6.7.4. The rebuilt package is
  `/nix/store/2y655l0mjv6aipmms1m174k1lxzqva1z-bluedevil-floss-6.7.90-c15cdd83448c`.
- With a mock Floss adapter/device, the real KDE backend passes discovery,
  device-property updates, SSP confirmation, bond completion/automatic connect
  dispatch, disconnect/forget, power actions, callback spoof rejection, and model
  reset after daemon disappearance. These are protocol/state tests, not pairing
  with a physical device.
- NixOS module evaluation passes with Plasma and the host's ALSA, 32-bit ALSA,
  JACK, PulseAudio compatibility and WirePlumber settings.
- The documented vendored-flake layout passes evaluation in a minimal host
  fixture. Generated manager, adapter, codec, automatic-audio, reconnect and pairing units pass
  `systemd-analyze verify`;
  this does not start services or prove sandbox compatibility with hardware.
- The real startup-fixed Floss daemon initializes against a scripted HCI
  controller through test-only socket interception: it exposes the simulated
  controller address, profile UUIDs, initialized media APIs and an explicitly
  inactive HFP configuration, an empty connected-audio snapshot, and an unready
  A2DP PCM response without an authorized lease. The real bridge reaches those APIs and rejects
  an unconnected address for both HFP and A2DP. This checks startup and actual
  D-Bus contracts; it does not emulate a remote headset or kernel HCI ownership.

## Runtime defects fixed

`btadapterd --help` crashed with SIGFPE before reaching its argument handling in
an isolated no-hardware sandbox. This was a real runtime blocker despite successful
compilation/linking. The cause was static initialization order of the Linux
property map. A construct-on-first-use fix passes a regression that reproduces
the original crash, including concurrent property access. The replacement
daemon builds and all three executables pass `--help` without a mock.

The no-controller check then exposed a separate shutdown race: process exit ran
C++ global destructors while a native alarm thread was still active. The Rust
shutdown path now avoids running C++ global destructors after its best-effort
cleanup. The final package passes both missing-controller shutdown
(exit 0 without timeout or signal) and initialized-controller SIGTERM shutdown
(exit 0 in 1.368 seconds).

Final tested Floss package:
`/nix/store/8mvclv74k6f1k89wq9kvfp81w48gz9ic-floss-0.1-745ee92b87ed`.
The workspace `result/` has been refreshed to this package and the corrected
BlueDevil, PipeWire, WirePlumber and AAC codec outputs; do not use earlier builds.

## Not established

Real controller access, production systemd sandbox operation, pairing,
codec negotiation, actual A2DP playback, HFP microphone transport, timing quality
and long-running stability require further testing. Private-session or mocked
checks cannot establish these results.

In the hardware-free network namespace, the manager's controller-monitor task
panics because the Bluetooth socket family is unavailable; its D-Bus API remains
responsive. That scenario validates the manager API only, not controller
discovery or recovery. The scripted HCI check starts the adapter daemon directly.

The existing `/home/gotlou/nixos` evaluation stopped at a missing Stylix/base16
store derivation before any Floss edits. The cloned configuration is unchanged;
`INST.md` describes the intended integration rather than claiming a tested full
host activation.

Logs and machine-readable results are in `runtime-results/{aac,auto-bridge,audio,bridge,kde,rfkill,hci,floss,systemd,socket-registry,socket-authorization-before}/`.
Reusable runners and mock sources are in `tools/`, with instructions in
`tools/ISOLATED-{AAC,AUTO-BRIDGE,AUDIO,KDE,HCI}-SMOKE.md`. Runtime reports are local artifacts
ignored by Git. No mock, socket shim or test environment is enabled by the
production NixOS module.
