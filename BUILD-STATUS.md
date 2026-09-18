# Build status — 2026-09-17

The AAC and automatic-audio stack builds successfully from the portable flake.
The final packages pass the isolated device matrix, real AAC encode/decode,
actual daemon startup/API/shutdown checks, and static systemd unit verification.
No host service was activated and no physical Bluetooth device was accessed.

The current pass builds from pinned remote sources plus portable patches.
No installed Bluetooth/audio services have been started or changed. Subsequent
unprivileged runtime checks are tracked in [RUNTIME-STATUS.md](RUNTIME-STATUS.md);
they found startup defects that compilation alone did not catch. The output
paths below include all runtime fixes and match the current `result/` link.

| Component | Result |
| --- | --- |
| BlueDevil | Built successfully, including applet/KCM battery and rename controls, Floss QML plugin, background reconnect/pairing workers and scoped rfkill controls. |
| PipeWire | Built successfully, including `pw-floss`. |
| WirePlumber | Built successfully after adding its Python build dependency. |
| Floss | Clean build passed, including AAC-enabled native stack and linked `btadapterd`, `btmanagerd`, and `btclient`. |
| AAC codec service | Built successfully; real encode/decode and malformed-input checks pass. |
| Flake/module | `nix flake check --no-build` passed; module evaluation has no failed assertions and no BlueZ daemon service. |
| Combined stack | `nix build ...#stack` passed; workspace `result/` links all four packages. |

Built output paths:

- BlueDevil: `/nix/store/2y655l0mjv6aipmms1m174k1lxzqva1z-bluedevil-floss-6.7.90-c15cdd83448c`
- PipeWire: `/nix/store/2v9hcni978hrmhrr4qi1iyvvrjcy51za-pipewire-floss-1.7.0-floss-744a6b547ddb`
- WirePlumber: `/nix/store/lz2ywmc71n8if5dlyz86k2s48vy7296c-wireplumber-0.5.17-floss-2649ebb4dc37`
- Floss: `/nix/store/8mvclv74k6f1k89wq9kvfp81w48gz9ic-floss-0.1-745ee92b87ed`
- Floss native archive: `/nix/store/k7k6fh4jf15iza2mqi8490dsd3m3j4cq-floss-native-0.1-745ee92b87ed`
- AAC codec service: `/nix/store/69954hxw6hlf99rx3fb54kd5b1n51av9-floss-codec-0.1-745ee92b87ed`
- Combined stack: `/nix/store/lrjnfmlnrdnsii6gawka8ydfywnqmvci-floss-desktop-stack`

PipeWire's upstream build compiles some uninstalled test binaries despite its
tests option being disabled; none were executed. Compilation is not evidence
of functioning HFP transport, codec negotiation, pairing or long-call stability.

Floss's clean native compilation, archive construction and Rust executable
linking passed. Build fixes include
missing C++ headers and declared D-Bus dependencies, compatible generated Rust
bindings, and callback ownership errors. The Nix package builds the native
archive separately and materializes its object members so Rust linking does
not depend on files left in the native build directory. Linking succeeds without
the upstream permissive duplicate-symbol linker option. Android code generators
and Chromium support dependencies also built from pinned sources.

Subsequent runtime checks fixed Linux property initialization order, a native
shutdown/destructor race, and an applet property unsupported by pinned Plasma.
The rebuilt outputs pass the isolated checks in [RUNTIME-STATUS.md](RUNTIME-STATUS.md).

See [INST.md](INST.md) for your kratos deployment instructions. These results
establish buildability, not successful operation with a headset or controller.

The final native archive also passes a symbol audit confirming that both AAC
and SBC encoders publish their actual PCM format to the automatic bridge. The
generated-flags dependency is explicit in the standalone native build.

The desktop update also adds `floss-policy.service` and mirrors compatible
BlueZ service restrictions (`MemoryDenyWriteExecute`, `ProtectProc=invisible`,
`SystemCallFilter=@system-service`) for Floss's manager, adapter and codec.
The final standalone and vendored Plasma configurations pass evaluation; all
six generated system/user units pass static verification. No services were
activated. PipeWire, WirePlumber and native Floss outputs are unchanged; the
Floss Rust package was rebuilt with the scoped rfkill API and authenticated
SocketManager callback ownership.

The current BlueDevil package passes 13 isolated desktop scenarios, including
background pairing ownership/handoff and five rfkill cases. Six daemon rfkill
unit tests pass with synthetic sysfs and an injected writer. The real rebuilt
Floss daemon also passes private-bus and scripted-controller checks. See
[RUNTIME-STATUS.md](RUNTIME-STATUS.md) for their scope and limitations.

The subsequent socket ownership fix builds successfully using cached dependencies.
The real rebuilt adapter rejects foreign callback IDs across all 16 protected
SocketManager methods. Two focused registry tests also pass; the previous
package reproduces the cross-client callback collision. The final standalone
and vendored flake checks and all six generated unit checks pass again.
