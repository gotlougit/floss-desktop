# Isolated audio smoke check

Run against built package directories without activating NixOS services:

```sh
python3 tools/smoke-audio-isolated.py \
  --pipewire /path/to/built-pipewire-package \
  --wireplumber /path/to/built-wireplumber-package \
  --output /tmp/floss-audio-smoke-report
```

The runner creates a private runtime/config/state directory and a private D-Bus
instance with no service activation directories. Both system and session bus
addresses point there. WirePlumber loads its policy-only profile, which has no
hardware monitors. The runner terminates every subprocess on exit and removes
the private runtime directories; logs and summary remain at `--output`.

On 2026-09-17 the built PipeWire and WirePlumber packages passed:

- Private PipeWire startup and WirePlumber policy initialization.
- `wpctl` and `pw-dump` communication with that private graph.
- Recognition and default selection of synthetic Floss-class sink/source nodes.
- Rejection of manual PCM-rate overrides; the daemon supplies negotiated formats.
- Explicit failure when the private bus has no Floss daemon.

Expected realtime portal/RTKit warnings occurred because those services were
absent from the private bus. No hardware monitor nodes appeared.

The synthetic nodes come from `pw-loopback`, not a Floss PCM transport. This
check does not establish that the daemon, bridge streaming, resampling control,
Bluetooth codecs, microphone transport, controller or headset work. Those need
separate integration/hardware validation. It does not exercise host session
configuration or change host services.

## Real bridge with private PCM peers

`smoke-bridge-isolated.py` exercises the built `pw-floss`, PipeWire, and
WirePlumber binaries together, substituting a small C/libdbus fixture for Floss
media D-Bus and PCM endpoints. Compile the fixture using existing development
tools (the recorded run used already cached Nix GCC/libdbus, with no downloads):

```sh
cc -Wall -Wextra -Werror -O2 tools/mock-floss-media.c \
  $(pkg-config --cflags --libs dbus-1) -o /tmp/mock-floss-media
python3 tools/smoke-bridge-isolated.py \
  --pipewire result/pipewire --wireplumber result/wireplumber \
  --mock /tmp/mock-floss-media --output runtime-results/bridge
```

This requires Python 3, bubblewrap, dbus-daemon and permission to create
unprivileged user namespaces. Bubblewrap supplies separate network, PID, IPC
and mount namespaces, an empty `/sys`, synthetic `/dev`, private `/var` and
`/run`, no capabilities, and no host audio or bus sockets. Only the report
directory is writable outside that namespace. No sudo or service activation
is used. A restrictive outer sandbox may need to allow launching bubblewrap;
the checks themselves still run unprivileged and isolated.

The completed 2026-09-17 run checked:

- Real HFP sink/source creation and removal in the private PipeWire graph.
- Ten seconds of bidirectional HFP PCM: nonzero playback reached the mock,
  and the recorded microphone signal retained the fixture's 500 Hz tone
  (499.99 Hz measured, S16 RMS about 11941).
- Rejection of a duplicate bridge while the first remains active.
- Unsupported HFP format, generation change while connecting, PCM socket
  loss, and D-Bus owner loss produce errors and remove bridge nodes.
- Four seconds of A2DP playback delivered nonzero PCM through the real bridge
  with asynchronous transport-position feedback; a regressing position
  counter caused an explicit error and teardown.
- The mock validates the device, profile, callback path, unique bus sender
  and lease token. Every tested exit calls `StopAudioSession` with that lease.

The fixture deliberately retains its unique bus connection after releasing
its well-known name in the owner-loss case, so cleanup can still be observed.
Reports are in `runtime-results/bridge/`; `summary.json` is written only after
all cases pass. The runner terminates all children even on assertion failure.

These checks validate bridge behavior against a controlled implementation of
the intended contract. They do **not** execute Floss codecs, validate the real
Floss daemon's matching behavior, or prove controller/headset interoperability,
long-term clock stability, RF performance, or sound quality. The fake PCM clock
is software generated; hardware audio still needs a separate test.

Automatic device/profile lifecycle coverage is described in [ISOLATED-AUTO-BRIDGE-SMOKE.md](ISOLATED-AUTO-BRIDGE-SMOKE.md). The manual bridge fixture now supplies GetA2dpPcmConfig; it never infers the feeder format from advertised codecs.
