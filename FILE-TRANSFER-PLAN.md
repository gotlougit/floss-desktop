# Floss file transfer: implementation plan

File transfer is not implemented in the current packages. This plan records
the local-source investigation and the boundaries that an implementation needs.

## Reuse and dependencies

Floss's `SocketManager` interface on
`/org/chromium/bluetooth/hci0/adapter` already supplies secure RFCOMM service
connections and listeners. Registered callbacks deliver connected Unix file
descriptors. See `Bluetooth/system/gd/rust/linux/service/src/iface_bluetooth.rs`
and `Bluetooth/system/gd/rust/linux/stack/src/socket_manager.rs`.

The Object Push UUID is `00001105-0000-1000-8000-00805f9b34fb`. Native SDP
registration already handles its OBEX descriptors and supported-format fields
in `Bluetooth/system/btif/src/btif_sock_sdp.cc`. Floss can continue to own
authentication, pairing, SDP, RFCOMM and all radio transport.

The Android OPP application is present, but depends on Android framework
services and the external `com.android.obex` implementation, which is absent
from this checkout. Its presence does not provide a Linux OBEX service.

Cached OpenOBEX 1.7.2 provides `OBEX_TRANS_FD` and
`FdOBEX_TransportSetup()`. The cached shared library has no libbluetooth runtime
dependency; its Nix recipe does have a BlueZ build input. An FD transport would
not require the BlueZ daemon or its D-Bus API. This is an old C parser, however,
not Android's OBEX implementation. Dependency selection and parser review remain
part of the implementation work. No dependency was added or downloaded for
this investigation.

## Sequence

1. Enforce SocketManager callback ownership against the authenticated D-Bus
   sender. Integer callback IDs and callback object paths are not credentials.
   Test two callers using the same callback path and attempts to operate on
   each other's IDs. Socket delivery, unregister, accept and close must remain
   isolated between clients.
   The current source implements this prerequisite; build and runtime evidence
   are tracked in [RUNTIME-STATUS.md](RUNTIME-STATUS.md).
2. Add a separate outbound transfer executable. Use Floss's secure RFCOMM
   connection and an OBEX FD transport, bounded streaming buffers, asynchronous
   progress, cancellation, and connection/transfer deadlines. Do not block the
   Plasma UI thread or load the parser into Plasma.
3. Integrate file selection and transfer status into BlueDevil's existing UI.
   A successful D-Bus request is not a successful transfer: report completion
   only after the peer accepts the final OBEX response.
4. Add an incoming Object Push listener with explicit per-transfer consent.
   Treat filenames, length headers and MIME types as untrusted. Create exclusive
   temporary files in the selected destination, reject traversal, bound resource
   use, handle collisions without overwriting existing files, and atomically
   publish completed files. Remove partial files on cancellation/error.
5. Package and sandbox the transfer process separately, granting destination
   access only as needed. Re-arm Floss's listener after each accept and recover
   cleanly from daemon replacement. Do not advertise OBEX FTP browsing.

## Required evidence

Use a private D-Bus service and socketpair OBEX peer to exercise the actual
parser and executable: multiple files, empty files, large streamed files,
partial writes, peer abort, cancellation, truncated/malformed headers, daemon
loss, callback spoofing, unsafe names, duplicate names, disk errors and cleanup.
Verify that malicious or stalled peers cannot block the UI or grow memory
without a bound. Add packaged UI checks and module evaluation after integration.

Hardware validation remains necessary for SDP discovery, secure RFCOMM
interoperability and transfers to real phones. Private socketpair tests cannot
establish those results.
