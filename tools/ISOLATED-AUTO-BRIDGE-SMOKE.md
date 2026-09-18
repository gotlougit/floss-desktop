# Automatic bridge policy/device matrix

Compile the scripted peer using an existing C compiler and libdbus development
headers (no toolchain download is performed by these commands):

```sh
cc -Wall -Wextra -Werror -O2 tools/mock-floss-auto.c \
  $(pkg-config --cflags --libs dbus-1) -o /tmp/mock-floss-auto
python3 tools/smoke-auto-bridge-isolated.py \
  --pipewire result/pipewire --wireplumber result/wireplumber \
  --mock /tmp/mock-floss-auto --output runtime-results/auto-bridge
```

The test starts actual PipeWire, WirePlumber policy, `pw-floss --auto`, and
ordinary `pw-cat` playback/recording clients in bubblewrap with private D-Bus,
PCM sockets, configuration/state directories and an empty hardware namespace.
It never connects to the host audio graph, system bus, or Bluetooth adapter.
Only the output report directory is writable outside the namespace. Children
are stopped on success or failure; `summary.json` is written only on full success.

The device matrix includes SBC-only capability advertisement, AAC capability
advertisement with a different negotiated feeder rate, stereo and mono playback-only speakers,
a CVSD 8 kHz headset, an mSBC 16 kHz headset, and an HFP-only device. The voice-only device must never request unavailable A2DP. For headsets it checks actual
client-triggered A2DP→HFP→A2DP changes, nonzero playback in both profiles,
microphone PCM retaining a 500 Hz tone after rate conversion, stable PipeWire node IDs, and recording restart debounce.
Capturing from an independent synthetic built-in microphone or opening a passive
headset level meter must not request HFP. Speakers must expose no phantom
microphone. The mono feeder must preserve the normal stereo application's 400 Hz playback tone. HFP admission denial must return to A2DP without rapid retries or
replacing node IDs. A delayed native teardown fixture rejects new reservations for 200 ms after stop; the bridge must wait without replacing nodes.

Lifecycle checks disconnect devices during playback and capture, reconnect,
remove/restore HFP capability, and replace the daemon bus owner while the same
automatic bridge process stays alive. A second device listed ahead of the active device must not steal its audio. Untargeted playback/recording clients exercise automatic headset defaults; an explicitly selected built-in microphone remains preferred across reconnection. The fixture verifies caller callback
identity and session lease tokens. It writes its actual reserve/start/stop
sequence to the report so profile decisions are checked independently of node
labels. `--cases sbc,cvsd` selects a smaller subset; available cases are
`sbc,aac,speaker,mono,cvsd,msbc,delayed,hfp-only,denied,lifecycle,defaults`.

`--bridge /path/to/pw-floss` can run an incrementally compiled bridge against
built PipeWire/WirePlumber packages while debugging; final verification should
omit it to exercise the packaged executable. Reports record the actual bridge executable path and SHA-256 hash in addition to package paths.

**Scope:** all Bluetooth device capabilities and PCM peers here are scripted.
Advertised AAC does not prove real AAC negotiation, encoding or decoding.
Actual Floss codec tests are separate. These checks do not exercise radios,
physical headset interoperability, hardware clocks, or controller SCO support.
The independent microphone is synthetic, not the host ALSA device.
