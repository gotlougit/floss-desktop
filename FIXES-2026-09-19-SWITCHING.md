# Speaker disappearance and output switching

Three independently actionable defects were found after the AAC fix:

- `pw-floss` treated more than 100 ms of transport backlog as a fatal error,
  destroying its nodes and making desktop policy fall back to another sink.
  Graph queue overflow did the same thing. Brief overruns now discard the
  incoming quantum while retaining bounded queues and the existing nodes.
  The two-second transport/position watchdog remains in place for real stalls.
- Profile transitions closed PCM before calling `StopAudioSession`. Closing
  the socket already requests a native stop; the subsequent lease stop could
  encounter a suspending stream and trigger the native profile-disconnect
  fallback. Lease release now precedes socket closure, as it already did in
  the final shutdown path.
- PipeWire audioconvert's disconnect fade read the previous input buffer after
  it had been returned. Live crash stacks faulted in that read. Ramp-only gap
  processing now copies its history while the buffer is valid; disconnect
  only marks the fade using that owned history. Gap-detection mode already
  maintained history while processing.

Regression coverage uses private D-Bus/PipeWire/WirePlumber instances and no
Bluetooth hardware. The old bridge fails the 350 ms mock transport-stall test
by replacing its endpoint; the updated bridge retains its node IDs across
three stalls in both A2DP and HFP mode. Eight PulseAudio suspend/resume and sink-move cycles also retain
nodes. AAC-capable, CVSD, and mSBC mock devices exercise playback, microphone
profile switching, passive meters, and return to A2DP. These are PCM/API tests,
not radio or codec interoperability tests.

`tools/test-audioconvert-disconnect.py` compiles the production gap code and
extracted disconnect callback with an inaccessible returned-buffer fixture.
The original code crashes with SIGSEGV; the corrected code preserves the last
samples and passes without accessing the returned buffer. Both initial and normal ramp states are covered.

Package builds and Nix evaluation use `--offline`. No system rebuild, sudo, or
Bluetooth daemon restart is performed by this fix. Real speaker confirmation
is still necessary before calling the deployed stack stable.

The mock also asserts that the PCM peer has not hung up when StopAudioSession
arrives. The old bridge fails that assertion during a microphone transition;
the corrected bridge passes. The packaged lifecycle/default-routing matrix
passes disconnects during playback and calls, capability changes, daemon
restart, second-device arrival, and preservation of explicit user preferences.

Live verification: after the speaker completed its reconnect following bridge
replacement, three silent 48 kHz PulseAudio playback/pause cycles to WILLEN II
kept the same speaker endpoint and default output. The fixed user bridge was
left running; its temporary systemd drop-in was removed and the user manager
reloaded so it cannot override a later NixOS deployment. This live check does
not establish audible quality or long-term reliability. The full offline
PipeWire build, flake checks, and final packaged jitter/pause regressions pass.
