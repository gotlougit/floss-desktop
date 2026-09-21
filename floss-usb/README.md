# Floss userspace USB transport

`floss-usb` replaces the USB transport for **one Realtek 0bda:c123 adapter**.
It uses the stock `hci_vhci` module and libusb; no kernel patch or BlueZ daemon
is involved. The service has been built and mock-tested, but has not yet been
activated against the physical radio. Working headset calls are not yet verified.

## Data path

```text
Floss HCI user channel <-> stock virtual hci0 <-> floss-usb <-> USB radio
                                                            <-> Bluetooth headset
```

The Rust code frames HCI packets, tracks SCO handles and connection events, and
orders endpoint changes before reporting connection success to Floss. A small C
libusb boundary owns asynchronous control, bulk, interrupt and isochronous
transfers. Bluetooth profiles, pairing, keys and audio codecs remain in Floss.
PipeWire continues to exchange PCM with Floss through `pw-floss`.

The helper first lets the stock btusb driver initialize the controller firmware
by opening and closing its HCI user channel. It then claims both physical USB
interfaces and creates virtual hci0. Floss's manager and adapter use that existing
HCI API without a replacement D-Bus stack or HCI HAL. Closing the service
unregisters the virtual device, cancels outstanding USB transfers, returns the
isochronous interface to alternate 0, and reattaches btusb. Startup also attempts
to reattach btusb after an unclean prior exit before initializing firmware again.
The radio must initially be unblocked and unused by another Bluetooth stack.

For hands-free audio, the service observes Synchronous Connection Complete,
legacy SCO Connection Complete, Disconnection Complete and host Reset packets:

- CVSD uses USB alternate setting 2 (16-bit host PCM).
- Transparent mSBC uses alternate 6 when available, otherwise alternate 1.
- This machine's Realtek descriptors expose alternate 1 with 9-byte USB packets
  and no alternate 6. Floss therefore needs **24-byte HCI SCO payloads**.
- `/run/floss-usb/sco-packet-size` publishes 24 or 60 (0 disables mSBC).
  The adapter launcher sets `FLOSS_HFP_MSBC_PACKET_SIZE` and enables Floss's
  software HCI path. Floss already implements 24-byte mSBC framing.
- SCO data travels in both directions. Unrelated disconnects and duplicate
  events do not stop the active SCO link. A reset cancels the old SCO transfers.

Only one SCO connection is accepted. Additional connections, unsupported air
modes, malformed/oversized packets, queue overflow and USB failures produce
explicit failures instead of unbounded allocation or silent data corruption.

## NixOS

Build `nix build .#floss-usb` and the updated Floss package, then add this option
alongside the existing Floss module configuration:

```nix
hardware.bluetooth.enable = true;
hardware.bluetooth.floss.usbTransport.enable = true;
```

The module supplies the service, stock kernel-module loading, udev rules,
restricted device access, service ordering and SCO packet-size configuration.
There is no per-application audio setup or manual bridge command. Do not set
`softwareHciTransport` separately for this backend.

For the first activation, reboot after deploying the configuration so the udev
permissions, stock virtual-HCI module and service ordering are all in place and
the previous adapter daemon has released the physical radio. **No custom kernel
build is needed.** Do not start this helper by hand while btadapterd owns the radio.

Inspect after deployment:

```sh
systemctl status floss-usb.service btmanagerd.service btadapterd@0_0.service
journalctl -b -u floss-usb.service -u btadapterd@0_0.service
journalctl --user -b -u floss-audio.service
```

A hands-free test should show the selected SCO USB alternate setting, advancing
RX/TX byte totals on disconnect, and actual intelligible microphone and playback
audio. Selecting a profile or seeing an HCI connection alone is insufficient.
The existing `pw-floss` missing-PCM guard remains enabled.

Disable `hardware.bluetooth.floss.usbTransport.enable` and reboot to return to
the regular btusb path. This service does not modify pairing databases.

## Validation and boundaries

`make check` (also run by the Nix package) needs no root, device, daemon or network:

- Rust tests cover controller selection, fragmented HCI packets, bidirectional
  CVSD/mSBC transport, command/ACL forwarding, reset, stale/duplicate events,
  failed connections and malformed input.
- C tests execute the actual libusb callbacks and transfer builders against
  simulated Realtek descriptors. They cover alternate settings 1/2/6, SCO
  fragmentation, control transfers, RX rearming, cancellation, bounded transfer
  slots and short-transfer failures. libusb itself supplies transfer allocation;
  device I/O is replaced by fixtures.

The updated Floss HFP file is separately compiled against the cached native GN
build. Flake checks evaluate both module configurations and assert no kernel
patches are installed. These checks cannot establish real USB scheduling,
firmware persistence across driver handover, kernel/VHCI startup, production
sandbox behavior, suspend/resume, hardware rfkill behavior or long calls.
Those require deployment and physical testing. LE Audio/ISO, multiple adapters
and arbitrary vendor/controller support are outside this implementation.

References: [Linux VHCI](https://kernel.googlesource.com/pub/scm/linux/kernel/git/stable/linux/+/refs/tags/v6.18.52/drivers/bluetooth/hci_vhci.c),
[Linux btusb](https://kernel.googlesource.com/pub/scm/linux/kernel/git/stable/linux/+/refs/tags/v6.18.52/drivers/bluetooth/btusb.c),
and the fetched Android Floss HFP implementation.
