# Floss review fixes — 2026-09-10

This records the source-only repair pass. Later compilation and packaging
results are tracked in [BUILD-STATUS.md](BUILD-STATUS.md).

All ten findings in [the initial review](FLOSS-REVIEW.md) have corresponding
source changes. Separate agents implemented desktop, PipeWire, and native Floss
fixes. An independent reviewer inspected the combined changes and contracts;
a second reviewer inspected the clock controller against local PipeWire code.
No further concrete defect was identified in those source reviews. This is not
proof of compilation, runtime correctness, or hardware compatibility.

## Findings and changes

| Finding | Implemented repair |
| --- | --- |
| 1. Pairing/profile connection | Successful locally initiated pairing requests profiles. ACL state is labelled separately; Connect Profiles remains available. Accepted requests do not imply profile readiness. |
| 2. Wrong-headset A2DP operations | Daemon reservations bind address, profile, token and actual D-Bus caller. Native admission and start/stop verify the active peer. Busy requests cannot replace another listener; legacy media mutations respect reservations. |
| 3. Failed SCO listener | Listener-open failure propagates to immediate teardown of the exact SCO connection, without waiting for incoming packets. |
| 4. PCM EOF/closure | Unexpected closure invalidates readiness and queues address/generation-checked teardown on the stack thread. Readiness publication and cleanup account for UIPC locking. |
| 5. Cancellation before readiness | The bridge receives a reservation token before starting audio and cancels with it at every startup stage. Owner departure cancels; unused reservations expire. Exclusion persists until native terminal state. |
| 6. Clock adaptation | Separate bounded playback/capture PI controllers drive PipeWire adaptive PCM resampling. A2DP obtains scoped native byte/timestamp feedback asynchronously. Buffer limits, feedback deadlines and latency reporting are implemented. |
| 7. SPA chunks | Offsets are normalized modulo capacity, lengths bounded, wrapped payloads copied in spans, and incomplete frames rejected. Empty chunks can produce silence without mapped payload. |
| 8. Qualification SLC ordering | Cached accepted SLC state is updated before dependent qualification audio startup. |
| 9. Pairing response lifetime | Prompts retain identity through submission, rejection and terminal bonding. Ambiguous RPC outcomes prohibit blind resend while preserving cancellation. Stale completions cannot answer a newer prompt. |
| 10. Pairing owner handoff | Observers cache prompts with original expiry and reconcile bond state on takeover. Since prior submission cannot be discovered, takeover permits cancellation only; cancel/re-pair obtains a fresh answerable prompt. |

Additional desktop changes use unique callback paths, distinct bus connections
for pairing ownership, view lifecycle tracking, and incremental model updates.
The unsupported KCM handbook action was removed.

The independent follow-up review found and prompted repairs for uncertain
pairing-answer replay, HFP cancellation during codec negotiation without a
terminal event, and HFP policy rejection without a terminal event. Native
cancellation now stops the negotiation timer, ignores stale negotiation replies,
and reports terminal failure without duplicate timeout notification.

## Contracts and source evidence

The inspected D-Bus boundary agrees on reservation/start/stop signatures,
authenticated owner identity, Unix notification descriptors, A2DP position map,
and HFP format/generation map. The bridge pins the daemon's unique bus owner.
Old tokens cannot stop replacement sessions. A2DP cancellation before Started
may disconnect that address's profile to obtain a terminal event; reconnecting
profiles can be necessary afterward.

Clock feedback direction was traced through the local PipeWire adapter and
resampler implementations. The controller has anti-windup and a finite ±1%
correction range. This establishes an implementation path, not measured
stability. A2DP startup silence is preloaded only once and may drain during
idle; startup/resume underruns require measurement. Latency values are estimates,
and forced graph quanta exceeding the bridge's 100 ms capacity are unsupported.

WirePlumber's generic policy remains the consumer of ordinary sink/source nodes;
these fixes add no automatic Floss monitor or profile switching. Updated
repository notes document the new audio session contract.

## Remaining work and validation boundary

- Compilation, QML loading, D-Bus dispatch and cancellation races remain untested.
- Controller/kernel SCO delivery, startup behavior, sustained duplex audio,
  rate-controller tuning and actual latency need hardware/runtime validation.
- A2DP feeding rate remains an independently verified CLI assertion.
- Automatic profile switching, active-seat ownership, background pairing,
  deployment permissions and service integration remain unfinished.
- The reproducible standalone Nix package/module and remote source hashes
  remain pending. Portable patches are not a substitute for that package.

No compiler, build, tests, lint, patch-application check, network access, or
download was run during this fix pass. Cumulative patches were exported from
local Git diffs, including untracked source files, without staging or commits.
Their pinned base revisions, byte sizes and raw SHA-256 hashes are recorded in
[patches/sources.json](patches/sources.json). These are patch-file hashes, not
Nix fetched-source hashes. Builds and runtime validation await authorization.
