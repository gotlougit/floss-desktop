# Isolated rfkill tests

The manager's per-controller rfkill parser and event builder have self-contained Rust tests. They create temporary sysfs-shaped fixtures and inject a writer which records or rejects bytes. They never read host sysfs, open `/dev/rfkill`, or access a controller.

With a Rust compiler and C linker already installed:

```sh
rustc --edition 2018 --test Bluetooth/system/gd/rust/linux/mgmt/src/rfkill.rs -o /tmp/floss-rfkill-unit-tests
/tmp/floss-rfkill-unit-tests
```

If `cc` is not on PATH, pass `-C linker=/absolute/path/to/cc` to `rustc`. The recorded run used cached tools; compiler/linker paths and the tested source hash are in `runtime-results/rfkill/summary.json`, with test output in `runtime-results/rfkill/unit-tests.log`.

Six tests cover exact Bluetooth controller matching alongside WiFi and another controller, operation/type/index bytes, hardware block refusal, unnecessary-write suppression, missing/ambiguous/platform-only mappings, malformed/oversized attributes, denied writes and bounded enumeration.

`GetRfkillState` returns `available`, `hard_blocked` and `soft_blocked`. Unavailable state must not be interpreted as confirmed unblocked. Virtual adapter IDs are resolved through the manager's known physical HCI identity; retained identities can be queried while down. An identity that has never been discovered, or a platform-wide switch with no exact `hciN` name, is unavailable. The API deliberately does not guess a radio or issue `CHANGE_ALL`.

These tests do not validate kernel device permissions, physical killswitches, hotplug races or the activated systemd sandbox. The production writer is nonblocking and issues one packed 8-byte Linux `rfkill_event`; a short write or open/write failure is reported as failure. The UI must refresh state following a successful request because hardware can change independently.

## Kernel ABI review

The locally cached Linux 6.16.7 source archive was inspected without downloading or building the kernel. In `net/rfkill/core.c`, `rfkill_fop_write` lines 1304–1309 handles `RFKILL_OP_CHANGE` only when **both** the device index matches and the event type matches (unless the caller explicitly uses `RFKILL_TYPE_ALL`). This implementation always sends `RFKILL_TYPE_BLUETOOTH`, so a vanished Bluetooth index cannot redirect this request to WiFi. `rfkill_register` lines 1078–1080 assigns an incrementing index; ordinary unplug/replug does not recycle it.

The kernel still reports an accepted write when the matching device vanished before the write, and a hardware switch can independently change state. Therefore the return value means request acceptance, and the caller must refresh `GetRfkillState` before presenting an unblock as complete. This source review is not a physical hotplug or activated-sandbox test.
