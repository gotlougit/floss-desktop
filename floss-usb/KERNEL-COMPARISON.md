# Linux USB Bluetooth transport comparison

This review used the Linux stable **v6.18.52** sources:

- [`drivers/bluetooth/btusb.c`](https://kernel.googlesource.com/pub/scm/linux/kernel/git/stable/linux/+/refs/tags/v6.18.52/drivers/bluetooth/btusb.c)
- [`drivers/bluetooth/hci_vhci.c`](https://kernel.googlesource.com/pub/scm/linux/kernel/git/stable/linux/+/refs/tags/v6.18.52/drivers/bluetooth/hci_vhci.c)

## Findings and changes

- `hci_vhci` preserves one HCI packet per file read and accepts one packet per
  write. The helper's VHCI record framing matches this behavior. VHCI advertises
  synchronous flow-control support, so preserving complete SCO records is
  required.
- `btusb_open()` submits one interrupt and two bulk receive URBs. The helper
  submitted only one bulk receive transfer. It now keeps two active, reducing
  the receive gap while a completed transfer is delivered and resubmitted.
- Like `btusb_isoc_complete()`, ordinary ISO receive packet/transfer loss
  now drops the missing data and rearms reception. ISO transmit loss is counted
  without restarting the transport. Device removal, stalls, unexpected
  cancellation, and reliable control/bulk short transfers still fail explicitly.
  Loss counters are logged when SCO switches or stops.
- SCO reassembly now validates the active connection handle as soon as its
  header arrives, discarding malformed continuation fragments as
  `btusb_recv_isoc()` does. Unlike the kernel's HCI user-channel exception, the
  helper can validate because it tracks the single SCO connection itself.
  An already-partial frame may still be damaged by packet loss; native mSBC
  decoding/concealment must handle that. This is not lossless USB recovery.
- `btusb_switch_alt_setting()` maintains two isochronous receive URBs, as the
  helper already did, and clears partial SCO reassembly while changing alternate
  settings, as the Rust transport already did.
- For wideband speech on alternate setting 6, btusb alternates six and seven
  empty USB frames before each 63-byte HCI packet to produce the required
  7.5 ms cadence. The helper previously submitted every packet immediately. It
  now implements the same 7/8-frame transfer pattern. The target Realtek
  radio exposes alternate setting 1, so this change protects the supported
  fallback for other revisions rather than explaining the current radio alone.
  The kernel's device-specific `BTUSB_ALT6_CONTINUOUS_TX` exception is not
  implemented because this helper is restricted to the Realtek 0bda:c123.
- The helper's control, ACL bulk, and SCO isochronous endpoint directions and
  request framing match btusb. Both use one interrupt receive and two
  isochronous receives. The helper deliberately supports only the selected
  Realtek ID and HCI transports used by Floss; LE ISO and kernel power-management
  integration remain outside its scope.

## Focused offline checks

`make check` runs the Rust suite, which exercises exact ACL payload preservation, bounded host bursts,
fragmented SCO framing, two active isochronous receivers, two active bulk
receivers, alternate-1 mSBC splitting, alternate-6 7/8-frame
cadence, transfer cancellation, and short-transfer failures. These fixtures do
not simulate host-controller scheduling, suspend/resume, firmware survival, or
headset audio; those require the physical deployment test described in the
README.

## Additional static-review fixes

- Cancellation waits for an elapsed monotonic two-second deadline, including
  when unrelated completions or interrupts wake the event loop early. Pending
  callback memory remains owned until cancellation completes; timed-out cleanup
  exits rather than freeing callback targets still owned by libusb.
- Pool exhaustion returns ENOBUFS for bounded retry. Transfer/buffer allocation
  failure returns ENOMEM, ending the helper instead of retrying indefinitely.
- VHCI registration reports poll errors and shutdown promptly, retries transient
  reads, and retains a five-second startup deadline.
- An I/O failure observed while cancelling SCO prevents starting new transfers.

Mock tests cover allocation failures, 300 early event-loop wakeups (with and
without interrupts), cancellation deadline exhaustion and later completion,
failed submissions, ISO packet/transfer loss, simulated USB removal, every missing
9-byte fragment position within a 27-byte HCI SCO packet, and malformed/stale
SCO headers. These tests do not establish arbitrary-loss recovery or hardware
scheduler equivalence.
