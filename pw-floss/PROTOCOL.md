# Native Floss audio bridge

`pw-floss --auto --adapter 0` discovers connected media devices, publishes a
PipeWire sink and (when the device supports HFP) a persistent microphone source,
and recovers after device or daemon disconnection. The NixOS integration starts
it as a user service. No device address, profile, or PCM rate is needed.

Floss selects and runs Bluetooth codecs. PipeWire only mixes and resamples PCM;
this integration contains no BlueZ backend or Bluetooth encoder. Codec availability is
determined by the Floss build and headset negotiation, not PipeWire capabilities.

## Automatic profile selection

The first ready device is selected and remains selected until it disappears.
Starting an active recording routed to that headset's microphone activates HFP.
The bridge watches links and consumers rather than source scheduling alone:
monitor streams and passive level meters do not request HFP. Recording
from another source does not activate the headset. After two seconds with no
headset capture demand, the bridge restores A2DP. Playback-only speakers expose
no microphone. Voice-only devices stay in HFP. Nodes retain their identities
across profile changes, so applications can keep their selected source/sink.
Session priorities prefer a connected headset over ordinary built-in devices;
WirePlumber still honors an explicitly saved user default.

The playback endpoint always exposes FL/FR stereo, including while the transport
is mono. The bridge averages the two PCM channels for HFP or a mono A2DP peer.
This prevents WirePlumber from restoring a mono channel map onto stereo playback;
legacy single-channel volume state is repaired by copying its gain to both sides.
Intentional stereo balance is preserved.

PipeWire renegotiates each retained node's sample rate and sample width when profiles change. The
bridge waits for the corresponding format callback before consuming PCM; the
dormant microphone supplies silence while A2DP is active or SCO starts. HFP
startup failure restores A2DP without deleting nodes, then retries after ten
seconds if microphone demand remains. If SCO connects but no microphone PCM
arrives within two seconds, HFP is suppressed for that bridge connection and
music is restored; this prevents repeated silent mode switches on unsupported
host transports. Reconnecting the device or restarting the bridge allows a
new attempt. A device loss or unrecoverable transport
failure removes nodes and re-enumerates after a two-second backoff.

The bridge does not pair or connect Bluetooth devices; BlueDevil handles that.
The global `org.pipewire.FlossAudio` bus name prevents competing bridges from
using Floss's singleton PCM sockets. It does not implement active-seat ownership.
One connected device is used at a time.

## Desktop profile controls

A standard PipeWire Device publishes profiles to PulseAudio and KDE's existing
sound settings. No plasma-pa fork is needed:

- **Automatic music / headset** follows real microphone demand.
- **High Fidelity Playback (SBC/AAC/aptX/aptX HD/LDAC)** requests that codec through Floss and
  keeps music mode even if an application opens the microphone. The persistent
  microphone endpoint supplies silence in this mode.
- **Handsfree Headset** keeps HFP active, including with no recording application.
  Floss negotiates CVSD or mSBC; there are no separate forced HFP codec choices.

The NixOS module also makes PulseAudio monitor streams passive. Opening KDE's
audio panel therefore does not wake idle ALSA devices and change the graph clock.
Meters follow an already running stream; an idle microphone can show no level.

Only choices supported by the device's negotiated capabilities are advertised. WirePlumber saves explicit
profile choices using its normal device-profile state. Codec selection is a
Floss negotiation request, not proof of the over-the-air codec: inspect the
Floss negotiation logs when verifying a real headset. Transport failures still
use the normal reconnect path.

## Authoritative format and lease contracts

All methods use `org.chromium.bluetooth.BluetoothMedia` on
`/org/chromium/bluetooth/hciN/media`. Calls are pinned to the daemon's unique bus
owner. Callback registration precedes the device snapshot; owner replacement
invalidates the connection and every old session.

- `GetConnectedAudioDevices() -> aa{sv}` returns ready devices, including
  `address`, `name`, `a2dp_caps`, and `hfp_cap`. Capabilities determine available
  profiles, never the PCM feeding format.
- `ReserveAudioSession(address: s, hfp: b, callback: o) -> t` reserves an
  address-scoped, caller-owned token before startup side effects.
- `StartAudioSession(token: t, callback: o, listener: h) -> b` accepts startup;
  the listener reports completion under a bounded deadline.
- `GetA2dpPcmConfig(token: t, callback: o) -> a{sv}` returns `ready: b`,
  `sample_rate: u`, `bits_per_sample: y`, `channels_count: y`,
  `generation: t`, and `socket_path: s`. Values come from the initialized native
  encoder feeder. This bridge supports packed S16LE/S24LE/S32LE, mono or stereo,
  at 44100/48000/88200/96000 Hz. Floss selects the actual valid codec format:
  aptX uses S16LE, aptX HD uses packed S24LE, and LDAC supports all three widths.
- `GetHfpPcmConfig(address: s) -> a{sv}` additionally returns `active: b` and
  `codec: u`. CVSD uses 8000 Hz and mSBC uses 16000 Hz, both S16LE mono.
- `StopAudioSession(token: t, callback: o) -> b` only releases that caller's
  lease. Disconnecting the private bus connection also cancels owned sessions.

The bridge confirms the configuration generation after connecting the fixed
PCM socket. A2DP generation changes trigger transport refresh; socket closure
invalidates HFP. Floss must be this integration: the bridge rejects missing format APIs
rather than guessing a sample rate. `--pcm-rate` is no longer accepted.

No kernel patches are supplied. HFP requires a controller/host transport that
carries SCO while Floss owns the HCI user channel. The optional
[floss-usb service](../floss-usb/README.md) supplies a userspace USB/VHCI path for
the supported Realtek adapter. The PCM-arrival guard detects missing audio and
restores music; it does not establish intelligible microphone audio. Physical
validation of the new transport remains necessary.

A diagnostic single session remains available:

```
pw-floss --adapter 0 --device AA:BB:CC:DD:EE:FF --profile a2dp
pw-floss --adapter 0 --device AA:BB:CC:DD:EE:FF --profile hfp
```

## Scheduling and recovery

Processing and transport transitions run on the non-realtime main loop, without
`PW_STREAM_FLAG_RT_PROCESS`. No other thread races the PCM rings or file
descriptor replacement. Socket IO is nonblocking and queues are bounded to
100 ms. Profile startup currently pauses this loop during bounded D-Bus/start
waits; it is not a hard-realtime SPA backend.

HFP drains microphone PCM continuously, including while idle, because incoming
SCO packets clock outgoing playback. Silence fills underflow. Separate bounded
queues preserve partial writes and reads; an incomplete sample is retained
across reads. Capture overruns discard old complete samples, log the overrun,
and retain the transport and desktop nodes. This bounds microphone latency when
the graph stalls. Socket failures still trigger transport recovery.

A2DP asynchronously polls owned native consumption position every 100 ms.
Adaptive PipeWire resamplers reconcile graph and transport clocks using bounded
PI controllers and a 30 ms buffering target. Invalid position, counter rollback,
or a two-second consumption stall invalidates the stream. Negotiated format and
device snapshots are checked periodically. ProcessLatency publishes measured
buffering plus reported remote A2DP delay; unknown HFP headset delay is excluded.

## Deployment boundary and remaining work

The NixOS module provides the service, D-Bus policy, and `bluetooth-audio` socket
access. Do not start a second raw socket client or run the bridge as root. Shared
Floss media initialization/cleanup belongs to the daemon, not this client.

Physical headset/controller support, SCO transport, reconnect timing and long
calls still require hardware validation. LC3 voice, LE Audio, simultaneous
headsets, AVRCP volume/media control synchronization, and active-seat arbitration
are not implemented. A2DP stays started while graph playback is idle.
