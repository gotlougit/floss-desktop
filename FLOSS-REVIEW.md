# Floss desktop implementation review — 2026-09-10

**Historical pre-fix review.** All ten findings received source fixes; see
[FLOSS-FIXES.md](FLOSS-FIXES.md) for the current status and remaining limitations.
The line references below describe the pre-fix code and may have shifted.

The architecture is plausible, but the reviewed implementation was not ready to
be treated as a working replacement. The review found defects in the normal
headset setup flow, audio ownership, and transport recovery. Fix the source
defects before spending data on a full build. A successful build would still
not establish stock-kernel SCO transport compatibility or reliable audio.

## Scope and evidence

Three agents independently reviewed native Floss, PipeWire/WirePlumber, and
BlueDevil. They reviewed implementations they did not author. A fourth agent
then audited the contracts across repositories and independently checked the
main integration findings. The primary agent checked supporting source and
consolidated this report. Agreement between reviewers is source evidence, not
runtime validation.

The reviewed snapshot preceded the fixes now exported in
[patches/sources.json](patches/sources.json), including the HFP changes. Review used local source, diffs, and references to
local APIs. No compiler, build, tests, lint, patch-application check, network
access, or dependency download ran. The implementation was not modified during
this initial audit; the findings below record its original conclusions.

P1 denotes a blocker for the intended usable integration. P2 denotes a
correctness or recovery defect that also needs repair, sometimes under a
narrower trigger. Known missing functionality and uncertain risks are identified
separately from verified implementation defects.

## Findings

### 1. P1: Pairing does not establish audio profiles, and the UI can hide Connect

[flossbackend.cpp:381](bluedevil/src/floss/flossbackend.cpp#L381) finishes a
successful pairing operation without calling `ConnectAllEnabledProfiles`.
Floss initializes `connect_to_new_profiles=false` in
[bluetooth.rs:398](Bluetooth/system/gd/rust/linux/stack/src/bluetooth.rs#L398),
and gates automatic profile connection on it at line 1314. Its reference client
explicitly connects profiles after bonding in
[callbacks.rs:297](Bluetooth/system/gd/rust/linux/client/src/callbacks.rs#L297).

If the bonding ACL remains connected and the headset does not initiate its own
audio profiles, BlueDevil marks the device Connected. The tray's
[DeviceItem.qml:30](bluedevil/src/applet/qml/DeviceItem.qml#L30) and the settings
page offer Disconnect instead of Connect. Neither A2DP nor HFP readiness follows
from that ACL state, so the bridge cannot start the expected audio session.

**Repair:** chain profile connection after successful locally requested pairing,
and represent profile readiness independently of ACL connectivity. Keep a way
to request profile connection when an ACL already exists. This is a behavior
defect beyond merely documenting that the current Connected label means ACL.

### 2. P1: A2DP selection failure can operate on another headset

[pw-floss.c:507](pipewire/src/tools/pw-floss.c#L507) accepts the empty reply to
`SetActiveDevice` and then invokes global `StartAudioRequest`. Floss silently
ignores a disconnected or unknown selected address in
[bluetooth_media.rs:3569](Bluetooth/system/gd/rust/linux/stack/src/bluetooth_media.rs#L3569).
The previous active headset can therefore be started. The selected-address check
only occurs afterward at bridge line 555; failure cleanup globally stops audio
at line 1027. A typo or unavailable requested headset is sufficient when another
headset remains active. Same-daemon replacement sessions also lack protected
cleanup ownership.

Native `StartAudioRequest` additionally replaces a pending notification listener
before deciding whether to accept a request. A false return does not guarantee
that no other client's operation was affected.

**Repair:** provide address- and session-scoped admission/start/stop, with an
explicit accepted request identity. Post-start address checking cannot undo
these side effects. The singleton bus name only coordinates cooperating bridge
instances; it does not make the Floss operation transactional.

### 3. P1: Failure to create the SCO PCM listener strands an accepted connection

[btm_sco_hci.cc:122](Bluetooth/system/stack/btm/btm_sco_hci.cc#L122) logs a failed
`UIPC_Open` and returns. `open_for_codec` advances generation and reports
`ready=false`, but does not disconnect the already connected SCO link.
Subsequent writes return early at line 199, never setting the transport failure
flag that would trigger the newly added exact-link cleanup.

A missing runtime directory, permissions problem, or resource exhaustion can
therefore leave SCO globally active and prevent subsequent bridge starts. The
bridge never acquired a ready generation and cannot safely identify it for stop.

**Repair:** propagate listener creation failure to `btm_sco_connected` and remove
that exact SCO connection immediately. Recovery must not depend on receiving
another SCO packet.

### 4. P2: Playback EOF can leave a ready snapshot for a destroyed PCM listener

[btm_sco_hci.cc:83](Bluetooth/system/stack/btm/btm_sco_hci.cc#L83) handles only
`UIPC_OPEN_EVT`. A client write-half shutdown, or an exit timed between successful
microphone delivery and playback reading, can make
[UIPC_Read:683](Bluetooth/system/udrv/ulinux/uipc.cc#L683) close both the accepted
socket and its listener. The close callback does not invalidate the PCM snapshot
or remove SCO. Later writes see `fd<0` and treat it as a client that has not yet
connected, rather than a terminated transport.

The generation can remain ready while no corresponding listener exists; SCO
remains active and prevents recovery. The send-failure cleanup added previously
does not cover this read-side closure path.

**Repair:** distinguish never-accepted, connected, closing, and failed endpoint
states. Invalidate readiness on unexpected close and marshal exact-link teardown
onto the stack thread. Do not write the stack-only failure flag from a UIPC
callback thread without synchronization.

### 5. P2: HFP startup cancellation can abandon its own live SCO session

[pw-floss.c:454](pipewire/src/tools/pw-floss.c#L454) records a generation only
after the ready-config poll. Cancel after SCO becomes ready but before that
poll: `b.generation` is still zero, and cleanup at line 1012 deliberately avoids
stopping a session whose ready state/generation differs from the pre-start
snapshot. No PCM client may ever connect, so socket failure handling cannot
recover it either. Later launches see an active session and refuse takeover.

**Repair:** return an ownership/request token at reservation time, retain it
through connection establishment, and support cancellation using that token.
The existing documentation acknowledges this gap; acknowledgment does not make
the shutdown path complete.

### 6. P1 completion gap: Independent audio clocks have no rate adaptation

[connect_stream:833](pipewire/src/tools/pw-floss.c#L833) creates graph-driven
streams. Incoming SCO PCM controls transport playback credit, while graph
processing produces playback and consumes capture at its own clock. Nothing
feeds their rate difference back into resampling or graph scheduling.

During sustained duplex operation with a nonzero clock mismatch, one queue
accumulates excess samples. The finite playback and capture queue limits at
[pw-floss.c:720](pipewire/src/tools/pw-floss.c#L720) and line 658 eventually
terminate the bridge. In other usage patterns, underflow is filled with silence.
Bounded buffering prevents unlimited memory/latency growth; it does not provide
clock synchronization. The exact time to failure and audible effects are
unmeasured.

**Repair:** implement transport clock recovery and bounded rate adaptation,
including latency accounting. Assess PipeWire's rate-control facilities against
the chosen scheduling design. This was documented as unfinished, but it blocks
claiming reliable long calls.

### 7. P2: Playback rejects valid SPA chunk offsets

[pw-floss.c:713](pipewire/src/tools/pw-floss.c#L713) interprets `chunk.offset` as
an absolute offset and requires contiguous payload before the allocation end.
The local [SPA buffer contract:53](pipewire/spa/include/spa/buffer/buffer.h#L53)
requires taking the offset modulo `maxsize`. Valid non-normalized offsets or
wrapped payloads can therefore terminate the entire bridge as invalid PCM.

**Repair:** handle zero capacity, normalize offsets, bound the payload size,
and copy wrapped data in two spans while preserving sample framing. Common
offset-zero buffers do not establish that the full API contract is satisfied.

### 8. P2: New SLC admission check breaks an existing qualification path

[bluetooth_media.rs:2706](Bluetooth/system/gd/rust/linux/stack/src/bluetooth_media.rs#L2706)
requires the cached HFP state to be `SlcConnected`. The existing SlcConnected
callback calls `start_sco_call_impl` for an active qualification call at line
1538, before updating that cached state at line 1573. The new guard rejects
this previously valid caller.

**Repair:** update accepted connection state before invoking dependent actions,
while preserving the callback's rejection rules. This is a narrower regression
in existing Floss behavior, distinct from ordinary manual desktop startup.

### 9. P2: Pairing responses discard the prompt before acceptance is known

[flossbackend.cpp:354](bluedevil/src/floss/flossbackend.cpp#L354) dispatches the
pairing response asynchronously, then immediately clears the prompt at line
357. Rejection or an RPC error only records a global error; the response UI is
gone, and its serial-based cancellation timer becomes ineffective while pairing can still
be active: the timer remains scheduled but its request-serial check no longer
matches.

**Repair:** retain request identity through response submission and rejection;
allow recovery or explicit cancellation without blindly resending an RPC that
may already have succeeded. Close the prompt on a confirmed outcome, and ignore
stale completions belonging to an older prompt.

### 10. P2: Pairing responder handoff loses an in-flight request

[flossbackend.cpp:299](bluedevil/src/floss/flossbackend.cpp#L299) discards prompts
in non-owner views. When the owner exits, lines 75–76 only acquire its session
bus name. A surviving settings view has no copy of the already broadcast
request, and Floss does not replay it.

**Repair:** give pairing requests a stable process lifetime or implement an
explicit handoff/reconciliation mechanism. Electing a replacement responder
does not transfer pending protocol state.

## Cross-repository contract results

| Boundary | Result |
| --- | --- |
| Media D-Bus names, paths, signatures and FD arguments | Consistent in inspected source |
| HFP `a{sv}` fields, codec IDs, mono rates, fixed socket path | Consistent across C++, Rust and C |
| HFP endpoint lifetime and cancellation | Inconsistent; findings 3–5 |
| A2DP selected-device ownership | Inconsistent; finding 2 |
| A2DP PCM feeding format | Not verified by API; still a manual CLI assertion |
| PipeWire nodes → WirePlumber discovery/default/linking | Coherent at source level; a PipeWire Device object is not required for this path |
| BlueDevil pairing → profile connection → bridge startup | Inconsistent; finding 1 |
| Inspected BlueDevil callback signatures and sender authentication | Consistent; pairing lifecycle defects remain |
| QML module declaration and imports | Consistent; installation/loading not validated |
| Service deployment, permissions, seat ownership and Nix packaging | Not implemented |
| Portable patch content, base revisions, raw sizes and hashes | Match the reviewed local snapshot; patch application was not tested |

## Engineering assessment and remaining uncertainty

Useful foundations include keeping Bluetooth codecs in Floss, matching typed
D-Bus projections, authenticating callback senders, bounded PCM queues,
generation checks, and serialized bridge processing. No additional verified
build dependency mismatch was found in the inspected replacement build paths;
that is not a compilation result.

The central engineering weakness is distributed state without an authoritative
session owner: pairing, ACLs, profile connections, pending audio requests,
accepted PCM connections, and graph nodes have different lifetimes. A getter
and cooperative lock do not replace a session protocol. Direct SCO access to
UIPC internals also spreads error handling between abstractions; explicit
nonblocking UIPC operations and lifecycle outcomes would reduce missed paths.

The desktop reviewer also identified a conditional multi-QML-engine risk:
callbacks use a PID-only path, while QML singletons are per engine. A second
backend in the same process can collide with the first, and its destructor can
unregister the shared path despite registration failure. The available local
source does not establish whether a normal Plasma configuration creates that
combination, so this is a conditional risk rather than a confirmed everyday
regression.

The software-HCI option skips custom management commands and supplies codec
capability records. It adds no stock USB driver's isochronous-interface setup.
Userspace code that can process SCO packets does not prove that the controller
transport delivers them. Kernel/controller compatibility, actual latency,
resampler behavior and headset interoperability remain unverified. Automatic
profile switching, active-seat ownership, and background pairing also remain
unfinished.

## Repair and validation order

1. Correct pairing-to-profile establishment and introduce explicit audio request
   ownership/cancellation semantics.
2. Make every native transport failure and close invalidate readiness and clean
   up its precise session; repair the existing-caller SLC regression.
3. Correct SPA buffer handling and implement audio clock adaptation.
4. Repair pairing response/handoff lifecycle and re-audit the resulting contracts.
5. When separately authorized, compile the selected revisions, validate the
   failure/cancellation paths and QML loading, then test SCO delivery and sustained
   duplex audio on the actual controller/kernel. Keep this staged before large
   build or VM downloads.

These are recommendations from the review, not changes or tests performed.
