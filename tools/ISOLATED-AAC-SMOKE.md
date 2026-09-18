# Real Floss AAC codec check

This runner starts Floss's actual `mmc_service` on a private D-Bus instance,
submits stereo PCM through its production codec socket protocol, and decodes
the resulting AAC LATM frames with FFmpeg. It checks the recovered 440 Hz tone,
not merely that some encoded bytes were returned.

Use already-built packages and an existing FFmpeg executable:

```sh
python3 tools/smoke-aac-isolated.py \
  --floss result/floss \
  --ffmpeg /path/to/ffmpeg \
  --output runtime-results/aac
```

The `--floss` directory can also be the independently built `floss.codec`
output: both expose `bin/mmc_service`. The runner needs Python, bubblewrap,
dbus-daemon and busctl. It does not fetch tools or dependencies.

The codec is tested at 44.1 and 48 kHz. Each case submits 30 frames, requires
decoded PCM with substantial signal amplitude, and checks that over 85% of its
energy matches the input tone after encoder priming. Invalid configurations and
truncated PCM must be rejected without killing the service. Repeated encoder
sessions check worker cleanup across profile turnover.

The runner uses private mount, network, PID and other namespaces, drops all
capabilities, and creates its own `/run/mmc/sockets`. It cannot reach a host
Bluetooth controller, service bus or audio graph. Only its report directory is
writable outside the namespace; no sudo or service activation is involved.

This tests real encoding and the MMC D-Bus/socket protocol. It does not exercise
remote codec negotiation, AVDTP packet delivery, a physical headset, or the
native Bluetooth daemon's MMC client. The Linux AAC source still advertises
44.1 kHz stereo: testing the encoder's 48 kHz capability does not change that
advertisement. See [../RUNTIME-STATUS.md](../RUNTIME-STATUS.md) for results from
the final packages.
