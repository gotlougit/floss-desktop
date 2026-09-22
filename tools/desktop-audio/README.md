# Desktop audio regressions

Use already available sources/development libraries. None of these tools fetch
packages or connect to the real audio server. The isolated runner uses private
D-Bus, PipeWire, PulseAudio, WirePlumber, XDG directories and synthetic devices.

## KDE

Apply `patches/plasma-pa-microphone-lifecycle.patch` to plasma-pa 6.7.5.
`build-microphone-test.py` compiles the actual modified `MicrophoneTest` class
with Qt Core and PulseAudio, replacing only device discovery and translations:

```
python3 tools/desktop-audio/build-microphone-test.py \
  --source /path/to/patched/plasma-pa --qt-base /path/to/qtbase \
  --pulse-dev /path/to/pulseaudio-dev --pulse-lib /path/to/pulseaudio \
  --output /tmp/microphone-fixture
```

Run it through the isolated runner with
`--cases mic-lifecycle --microphone-lifecycle /tmp/microphone-fixture/microphone-lifecycle`.
This tests cancellation before stream creation completes, device disappearance
while recording, UI-object destruction during playback creation, and normal
playback completion. The PulseAudio stream operations and Qt event loop are real.

For the panel's QML cancellation sequence, use
`--cases kde-cancel --plasma-pa /path/to/plasma-pa-package` and `--pulse-tools`.
The fixture repeats ten record/cancel/source-change cycles. The optional
`--legacy-kde-cancel` selects the old close sequence as a negative control:
on the installed unpatched KDE component it leaves replay streams behind and
fails. The corrected sequence calls `cleanupStreams()` directly and passes.

The lightweight class build does not replace a complete KDE application build.
