# Scripted HCI startup smoke check

This fixture drives the **real Floss adapter daemon** through its production
Linux HCI socket calls. An `LD_PRELOAD` shim redirects only AF_BLUETOOTH/HCI
socket creation and channel bind to private Unix packet sockets. A Python peer
answers an explicit list of controller initialization commands. Unknown
commands return `Unknown HCI Command`; there is no blanket-success fallback.

The fixture is deliberately smaller than a controller emulator: it has no
remote peers, Bluetooth links, radio, ACL/SCO traffic, pairing, or codec model.
It also bypasses Linux HCI channel ownership and physical USB transport. A
successful run establishes initialization and D-Bus exposure against scripted
responses; it cannot establish headset interoperability.

For the actual-daemon SocketManager ownership regression, compile
`tools/socket-auth/client.cpp` with `tools/socket-auth/build.sh`, supplying an
already cached C++ compiler, Qt base output and destination binary. Pass that
binary as `--socket-client /path/to/binary` to the runner. It retains two private
bus connections, tests all 16 foreign callback-ID methods, same-path identity
isolation, valid-owner dispatch, unregister/reconnect and forged disconnects.
It never creates an authorized listening or outgoing socket. The runner records
its result and binary hash in `summary.json`; the client refuses to run outside
the fixture's explicitly named private bus.

Build both helpers using existing development tools:

```sh
cc -shared -fPIC -Wall -Wextra -Werror -O2 \
  tools/mock-hci-socket.c -ldl -o /tmp/mock-hci-socket.so
cc -Wall -Wextra -Werror -O2 tools/mock-hci-check.c -o /tmp/mock-hci-check
```

First check the fixture independently of Floss:

```sh
python3 tools/smoke-hci-isolated.py \
  --floss result/floss --shim /tmp/mock-hci-socket.so \
  --self-check /tmp/mock-hci-check --output runtime-results/hci-fixture
```

That self-check passed on 2026-09-17: a C client invoked the production-shaped
Bluetooth socket/bind/write/read sequence, observed management enumeration of
index 0, received a successful HCI Reset response, and received an explicit
error for an unknown command. This result **does not run Floss**.

Run the actual adapter check with the corrected Floss package:

```sh
python3 tools/smoke-hci-isolated.py \
  --floss /path/to/built-floss --pipewire result/pipewire \
  --shim /tmp/mock-hci-socket.so \
  --output runtime-results/hci
```

Both modes require bubblewrap, Python 3, dbus-daemon, busctl, and permission to
create unprivileged user namespaces. The runner uses separate network, PID,
IPC and mount namespaces, removes all capabilities, supplies empty `/sys` and
synthetic `/dev`, and keeps D-Bus and controller sockets private. It does not
use sudo, activate NixOS services, or expose host controllers or audio sockets.
Only the report directory is writable outside the namespace.

The real adapter check passed on 2026-09-17 with package
`/nix/store/8mvclv74k6f1k89wq9kvfp81w48gz9ic-floss-0.1-745ee92b87ed`:

- Actual daemon and native stack initialized against the scripted controller.
- D-Bus exposed controller address `11:22:33:44:55:66` and profile UUIDs.
- Bluetooth media reported initialized; an absent headset correctly had no
  active/ready HFP PCM configuration. The connected-audio snapshot was empty,
  and A2DP PCM configuration was unready without an authorized session lease.
- Setting the adapter name through D-Bus round-tripped through native state.
- Actual `pw-floss` registered callbacks and attempted session admission with
  both HFP and A2DP. The real daemon rejected the disconnected address as
  expected, and remained alive.
- The initialized adapter exited with status 0 on SIGTERM in 1.368 seconds.
  The runner allows 20 seconds because upstream bounds its shutdown waits to
  4 seconds for disable, 11 seconds for cleanup, and a final 1-second delay.

The optional `--pipewire` argument enables those real bridge admission checks.
Native syslog goes to a private `/dev/log` receiver in `controller.log`.
Core dumps are disabled. Earlier fixture runs lacked the mandatory SSP feature
and LE Rand response; native diagnostics identified those fixture limitations,
which were corrected before the passing run. The final run uses the package
with the separately diagnosed startup initialization and shutdown destructor
race fixes. No additional daemon changes were needed for the HCI fixture.

`summary.json` appears only after every selected check passes. Fixture-only
results must not be described as a successful daemon startup. Successful
startup does not mean pairing, profile connections, or audio work against a
real controller; none of those are represented by this scripted peer.

## SocketManager caller authorization

Build the optional small Qt probe using already cached tools:

```sh
bash tools/socket-auth/build.sh /path/to/c++ /path/to/qtbase /tmp/floss-socket-auth-client
```

Pass `--socket-client /tmp/floss-socket-auth-client` to the HCI runner. The helper
is bound into its existing isolated namespace and refuses any bus address other
than the runner's private bus. It keeps two independent clients connected to the
actual adapter daemon, registers the same callback path under both identities,
and checks same-owner registration idempotency and cross-owner isolation.

Every callback-bearing SocketManager operation must return AccessDenied when
using the other client's callback ID. A malformed device map with a foreign ID
also checks authorization precedes typed device conversion. Legitimate owners
exercise Accept/Close only with nonexistent socket ID zero, and unregister only
their own callback. The test initiates no authorized listening or outgoing socket
operation. Disconnect/reconnect must not inherit the departed client's ID, and
removing one client's callback must preserve the other client's registration.

The result records all denied method names and the helper executable SHA-256.
These checks cover real exported RPC authorization and callback lifetime. They
do not demonstrate cleanup of live native listening/connecting sockets, since
no such sockets are created by this test.
