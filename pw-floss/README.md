# pw-floss

`pw-floss` is a standalone PipeWire PCM bridge for the Floss Bluetooth stack.
Floss owns Bluetooth profiles, codecs, packetization, and transport; this
program publishes ordinary PipeWire nodes and transfers interleaved PCM over
Floss's Unix sockets.

It is an external PipeWire client, not an in-process PipeWire plugin. Its only
direct runtime library dependencies are PipeWire and libdbus. See [PROTOCOL.md](PROTOCOL.md)
for the D-Bus, PCM, profile-switching, and recovery contracts.

## Build

```sh
meson setup build
meson compile -C build
meson test -C build
```

Install with `meson install -C build`. For normal desktop use, run one bridge
per user session:

```sh
pw-floss --auto --adapter 0
```

The Floss daemon must provide the audio-session and negotiated-PCM APIs
documented in `PROTOCOL.md`. A global D-Bus name prevents two bridge processes
from competing for the singleton Floss PCM transport.

## License

MIT, matching the PipeWire source tree from which the original bridge was
extracted. See [LICENSE](LICENSE).
