# Audio-panel meters and unwanted HFP switching

At 16:52:34 and 16:52:41 the bridge entered 8 kHz HFP while the user opened
Plasma audio settings; it returned to A2DP when the panel closed. Reopening
at 16:54:35 caused the same transition. The bridge did not restart.

The installed KDE PulseAudioQt VolumeMonitor passes stream flags 0x2a00:
PEAK_DETECT | ADJUST_LATENCY | DONT_MOVE. It omits DONT_INHIBIT_AUTO_SUSPEND.
PipeWire marked such streams stream.monitor=true but did not make them passive.
The source became STREAMING, which the Floss bridge interpreted as microphone
demand. The previous test explicitly supplied node.passive=true, missing this.

The PulseAudio server now sets node.passive=in-follow for peak-detection
streams as well as streams requesting DONT_INHIBIT_AUTO_SUSPEND. Meters follow
an independently activated source but cannot initiate or prolong HFP capture.
Ordinary recording streams retain their existing behavior.

The libpulse fixture in tools/pulse-peak-meter.c uses KDE's exact flags without
an application-name exception. The peak-meter case in the isolated harness
checks no HFP on meter creation, peak samples during genuine recording, and
return to A2DP while the meter remains open. The old package fails the first
assertion. No hardware is touched by this test.

The existing absent codec/profile menu is separate: pw-floss exposes sink and
source nodes, not a PipeWire Device with enumerable Profile parameters and
profile-selection actions. This change does not add AAC/SBC/HFP menu controls.

Validation: the corrected packaged server passes the libpulse regression.
It delivered 51 peak samples while genuine recording was active and returned
to A2DP with the meter still open. The offline package build and flake checks
passed. The installed KDE VolumeMonitor was also opened against the live
corrected server without causing HFP transitions.
