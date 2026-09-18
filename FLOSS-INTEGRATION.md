# Native Floss desktop integration

Build and NixOS deployment instructions are in [NIXOS.md](NIXOS.md).
Current compilation evidence is tracked in [BUILD-STATUS.md](BUILD-STATUS.md).
Unprivileged runtime and mock-device checks are in [RUNTIME-STATUS.md](RUNTIME-STATUS.md);
the `kratos` installation guide is [INST.md](INST.md).

These are Floss-only forks of BlueDevil, PipeWire, and WirePlumber. The desktop
UI and ordinary audio graph are retained, while BlueZ backends and dependencies
are removed. There is no runtime backend selector or BlueZ fallback. These
forks do not implement the BlueZ D-Bus API; unrelated applications that speak
that API still need porting.

SocketManager operations now require the authenticated D-Bus sender to own the
supplied callback ID. Registration keys include both the sender and callback
object path, so different clients can use the same path without sharing an
identity. This protects socket operations between authorized desktop clients;
it does not provide per-device Trust/Block or active-seat arbitration.

## Ownership

* Floss owns bonding, Bluetooth profiles, codec encoding, packetization, and HCI.
* BlueDevil owns the device UI and explicit user decisions about pairing and
  connections. The desktop integration must not reimplement pairing protocols.
* PipeWire owns the application audio graph and PCM mixing/conversion. Its Floss
  bridge feeds PCM to Floss; it does not use PipeWire's Bluetooth encoders.
* WirePlumber continues to manage the ordinary audio graph with its normal main
  profile. BlueZ-specific monitors and policy are removed. The user service
  launches `pw-floss --auto`, which discovers devices and switches profiles on
  headset microphone demand. Active-seat ownership remains separate work.

The Bluetooth checkout now carries explicitly authorized HFP transport/API
changes. BluezQt remains unchanged and is not a dependency of the revised
BlueDevil fork. Reusing its pure UI/model ideas does not require retaining its
BlueZ D-Bus implementation.

## First milestone

The implemented first milestone targets one adapter, basic discovery/bonding/device
control in Plasma, and one headset with either stereo playback or duplex HFP
voice audio. The repositories
carry implementation and deployment notes:

* [BlueDevil backend](bluedevil/src/floss/README.md)
* [PipeWire PCM bridge](pipewire/src/tools/pw-floss.md)
* [Floss HFP transport/API](Bluetooth/system/gd/rust/linux/HFP_PCM_BRIDGE.md)
* [WirePlumber integration](wireplumber/docs/rst/daemon/configuration/floss.rst)
* [Portable patches and base revisions](patches/README.md)
* [Review fixes and remaining validation](FLOSS-FIXES.md)

Consult the build and runtime reports for exact package results and isolated
checks. Physical controller/headset behavior remains unvalidated.

### BlueDevil

Floss is the only backend. `BLUEDEVIL_FLOSS_ADAPTER` chooses the adapter index
(default 0); set it consistently for Plasma and settings. The old
`BLUEDEVIL_BACKEND` selector is removed.

Expose a shared Qt/QML Floss model to the tray and settings. Adapt Floss callbacks
into device state and operation completion. Provide a single pairing responder
within the supported desktop session, including display-only PIN requests, and
authenticate callback senders against the daemon's current unique D-Bus owner.

Do not equate accepting a CreateBond/Connect request with completing it. Observe
the matching state callback, report errors/timeouts, and discard outstanding
work when the daemon goes away. Do not represent a locally stored boolean as
Floss-enforced device trust or blocking. Unsupported controls must be absent or
explicitly unavailable.

Successful locally requested pairing now requests profile connection. The UI
labels ACL connectivity separately and keeps an explicit Connect Profiles action
available; request acceptance does not claim per-profile readiness. Battery
reporting uses Floss's authenticated BatteryManager callbacks and snapshots,
including separate component readings. Device aliases use Get/SetRemoteAlias;
void setter success is followed by state confirmation before reporting success.
A background `bluedevil-floss-policy` process shares reconnect policy with the
UI: one elected owner, paired-only retries with increasing delay, persistent
Disconnect/Forget inhibition, and bounded cleanup of late connection completions.
This restores disconnected ACL links; it does not prove per-profile readiness
or recover an individual profile whose ACL link remains connected.
Trust/block controls and OBEX remain unimplemented. Rfkill state and scoped
software-unblock operations go through the manager; only mapped kernel hciN
switches can be targeted. The UI never equates request acceptance with an
unblocked radio, and hardware blocks remain visible.
The remaining daemon enforcement and transfer work is specified in
[DESKTOP-POLICY-PLAN.md](DESKTOP-POLICY-PLAN.md) and
[FILE-TRANSFER-PLAN.md](FILE-TRANSFER-PLAN.md).
Pairing ownership is elected
between views within one session, including the standalone
`bluedevil-floss-pairing` agent, which remains hidden until needed. Multi-session
arbitration remains follow-up work. Surviving observers retain
pending prompts, but handoff is cancel-only because Floss cannot report whether
the previous owner already answered. Cancel and re-pair to obtain a fresh prompt.

The original BlueZ wizard, background service activation, and OBEX integration
are removed. Pairing is exposed through the existing tray/settings UI. Floss broadcasts pairing
callbacks rather than implementing BlueZ's default-agent selection. Keyboard
PIN display may reflect a PIN already generated and submitted by Floss; the UI
must not send a second answer. Remote Just Works consent is handled by the
reference Floss stack itself, which rejects unsolicited consent requests before
notifying clients.

### PipeWire

The Linux build includes a standalone libpipewire PCM bridge and requires D-Bus.
BlueZ SPA plugins, Bluetooth encoders, and their dependencies are removed.
Separate D-Bus/control processing and potentially blocking I/O from audio
processing. Use bounded PCM buffering, handle partial socket writes, and stop
on loss of the selected device or daemon. The Floss socket is global rather
than per-device, so independent bridges must not compete for it.

The patched `GetA2dpPcmConfig(token, callbackOwner)` API reports the actual native
feeder format and generation only for the active, owned session. The bridge
checks that configuration around socket connection and during playback. Codec
capabilities and accepted preferences are not treated as negotiated PCM format.
AAC uses Floss's upstream MMC/FFmpeg encoder in a separate codec service; the
upstream source advertises 44.1 kHz stereo. SBC remains available.

The patched media API reserves an address-specific session with an authenticated
bus-owner token before starting native audio. StartAudioSession passes a
notification file descriptor, distinct from the PCM socket. Its immediate result
only accepts/rejects the request; streaming follows the asynchronous result.
StopAudioSession uses the token even before readiness, and daemon-side exclusion
lasts until terminal native state. Owner loss also cancels the session. The PCM socket path in this revision is
`/var/run/bluetooth/audio/.a2dp_data`; file permissions must allow the selected
audio session to access it without running the audio bridge as root.

The bridge is called `pw-floss`. Its default automatic mode snapshots connected
audio devices after registering callbacks, keeps one selected device, and
rediscovers after disconnects or daemon restarts. The NixOS module starts it as
a user service. No PCM-rate override is accepted. Explicit `--profile` and
`--device` arguments remain available for diagnosis.

### HFP microphone and playback

`pw-floss --profile hfp --device ADDRESS` selects duplex voice audio. This mode
uses Floss's existing `/var/run/bluetooth/audio/.sco_data` PCM socket and SCO/HCI
processing. It exposes an `Audio/Sink` for mono playback and an `Audio/Source`
for microphone capture. WirePlumber's generic audio policy can manage both;
BlueDevil does not need to carry audio or implement Bluetooth codecs.

The Floss patch adds `GetHfpPcmConfig(address)` to `BluetoothMedia`. It reports
the live native PCM configuration and a transport generation rather than
guessing the codec from advertised capabilities. The bridge confirms the
configuration around socket connection and accepts CVSD at 8 kHz or mSBC at
16 kHz, both signed 16-bit mono. mSBC encoding/decoding and loss concealment
remain in Floss; CVSD encoding is performed by the controller. LC3 voice remains excluded: enabling the AAC codec service does not implement
the separate LC3 voice configuration and transport contract.

SCO receive traffic clocks Floss's transmit path. The bridge must keep draining
microphone PCM even without a recording application and supply playback silence
when needed. Independent bounded rate controllers adapt playback and capture
using PipeWire PCM resampling. A2DP uses owned native consumption/timestamp
feedback. Queue limits and latency estimates are implemented; tuning, startup
behavior and reliable long-running operation remain unvalidated.

Automatic mode publishes a microphone only for HFP-capable devices. Recording
from that microphone starts HFP; a two-second idle debounce returns to A2DP.
Ordinary profile transitions retain sink/source identities and renegotiate their
formats. Failed HFP activation restores A2DP with retry backoff. Built-in
microphone recording and passive meters do not trigger the switch. Call buttons,
telephony state, hardware microphone gain and active-seat policy remain separate
integration work.

Floss's Linux HAL uses management commands `0x0100` and `0x0101` to query codec
capabilities and notify SCO state. A software-HCI fallback cannot establish that
a stock USB driver configures its isochronous endpoint correctly. Controller
and kernel transport support must be established before claiming working HFP
on a particular machine.
The Floss patch provides the explicit daemon environment setting
`FLOSS_HFP_SOFTWARE_HCI_TRANSPORT=true` for a transport already known to carry
SCO under userspace HCI ownership. It is disabled by default; enabling it does
not implement missing USB driver support.

### Service permissions

Callback registrations use client object paths and unique bus names. A Floss
daemon running as a dedicated user needs permission to send callback method
calls to those clients; ordinary D-Bus reply permissions are insufficient.
Client callbacks must reject callers other than the expected daemon owner.
The bridge also needs the documented singleton bus-name permission. Keep those
rules narrow instead of allowing arbitrary system-bus ownership or messages.

The implementation does not change the system service or D-Bus policy on this
machine. WirePlumber's normal profile does not install those permissions,
start Floss, or establish logind-based ownership of the PCM socket. The former
experimental `main-floss` profile is no longer needed.

## Remaining validation and extensions

Physical controller/kernel SCO transport, over-air AAC negotiation, long-call
clock behavior, hardware volume, active-seat ownership, multi-device routing,
LE Audio and OBEX still need work. The isolated test fixtures exercise actual
bridge code, codecs and controlled D-Bus peers; they do not reproduce a radio or
prove interoperability with every headset.

## Packaging and validation boundaries

The standalone flake and NixOS module fetch pinned remote sources and apply
portable patches, without using the reference checkouts as build inputs. The
module defines sandboxed services, D-Bus permissions, runtime directories and
desktop package selection. Compilation and isolated runtime testing are
authorized; no host activation or controller access has been performed.

Reusing Floss avoids writing new Bluetooth protocol implementations, but does
not establish security equivalence to Android's production configuration. The
Linux services and desktop clients need their own validation. See the linked
build/runtime reports for results rather than treating architecture or mocks
as evidence of hardware compatibility.
