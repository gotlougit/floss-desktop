#!/usr/bin/env python3
"""Exercise the real Floss MMC AAC encoder on a private bus/socket filesystem.

No Bluetooth controller or host service is used. This tests codec transport and
valid AAC frames, not over-air negotiation or headset compatibility.
"""
import argparse
import array
import json
import math
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--floss', type=Path, required=True)
p.add_argument('--ffmpeg', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--inside', action='store_true', help=argparse.SUPPRESS)
a = p.parse_args()
if not a.inside:
    a.output.mkdir(parents=True, exist_ok=True)
    (a.output / 'summary.json').unlink(missing_ok=True)
    command = [shutil.which('bwrap'), '--die-with-parent', '--unshare-all', '--cap-drop', 'ALL',
        '--tmpfs', '/', '--ro-bind', '/nix/store', '/nix/store', '--ro-bind', '/etc', '/etc',
        '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp', '--tmpfs', '/run',
        '--ro-bind', str(Path(__file__).resolve()), '/smoke.py',
        '--bind', str(a.output.resolve()), '/report',
        '--setenv', 'SMOKE_DBUS', str(Path(shutil.which('dbus-daemon')).resolve()),
        '--setenv', 'SMOKE_BUSCTL', str(Path(shutil.which('busctl')).resolve()),
        str(Path(sys.executable).resolve()), '/smoke.py', '--inside',
        '--floss', str(a.floss.resolve()), '--ffmpeg', str(a.ffmpeg.resolve()), '--output', '/report']
    sys.exit(subprocess.call(command))

Path('/run/mmc/sockets').mkdir(parents=True)
env = dict(os.environ, DBUS_SYSTEM_BUS_ADDRESS='unix:path=/tmp/private-bus')
Path('/tmp/bus.conf').write_text('''<busconfig><type>session</type><listen>unix:path=/tmp/private-bus</listen>
<policy context="default"><allow own="*"/><allow send_destination="*"/><allow receive_sender="*"/></policy></busconfig>''')
children, logs = [], []
def start(name, command):
    log = (a.output / (name + '.log')).open('w'); logs.append(log)
    process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
    children.append(process)
    return process

def call(method, data=None):
    command = [os.environ['SMOKE_BUSCTL'], '--address=' + env['DBUS_SYSTEM_BUS_ADDRESS'],
        '--timeout=3', '--json=short', 'call', 'org.chromium.mmc.CodecManager',
        '/org/chromium/mmc/CodecManager', 'org.chromium.mmc.CodecManager', method]
    if data is not None: command += ['ay', str(len(data)), *map(str, data)]
    return subprocess.run(command, env=env, capture_output=True, text=True, timeout=5)

def varint(n):
    out = bytearray()
    while n > 127: out.append((n & 127) | 128); n >>= 7
    out.append(n)
    return bytes(out)

def field(number, value): return varint(number << 3) + varint(value)
def message(number, value): return varint((number << 3) | 2) + varint(len(value)) + value

def init(rate, bits=16, channels=2):
    param = b''.join(field(i + 1, value) for i, value in enumerate([rate, channels, 192000, bits, 1000]))
    return call('CodecInit', message(1, message(5, param)))

def parse_response(response):
    assert response.returncode == 0, response.stderr
    raw = bytes(json.loads(response.stdout)['data'][0])
    position = 0
    def getvar():
        nonlocal position
        value = shift = 0
        while True:
            byte = raw[position]; position += 1; value |= (byte & 127) << shift
            if byte < 128: return value
            shift += 7
    fields = {}
    while position < len(raw):
        key = getvar()
        if key & 7 == 2:
            size = getvar(); fields[key >> 3] = raw[position:position + size]; position += size
        else: fields[key >> 3] = getvar()
    return fields[1].decode(), fields[2]

results = {'codecPackage': str(a.floss), 'ffmpeg': str(a.ffmpeg)}
(a.output / 'summary.json').unlink(missing_ok=True)
try:
    start('bus', [os.environ['SMOKE_DBUS'], '--nofork', '--config-file=/tmp/bus.conf'])
    deadline = time.monotonic() + 5
    while not Path('/tmp/private-bus').exists():
        assert time.monotonic() < deadline; time.sleep(.05)
    mmc = start('mmc', [str(a.floss / 'bin/mmc_service')])
    deadline = time.monotonic() + 10
    while call('CodecCleanUp').returncode:
        assert mmc.poll() is None and time.monotonic() < deadline; time.sleep(.05)
    for rate in (44100, 48000):
        path, frame_samples = parse_response(init(rate))
        assert frame_samples == 1024, frame_samples
        encoded = bytearray()
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as sock:
            sock.settimeout(3); sock.connect(path)
            for frame in range(30):
                pcm = b''.join(struct.pack('<hh', sample, sample) for index in range(frame_samples)
                    for sample in [round(12000 * math.sin(2 * math.pi * 440 * (frame * frame_samples + index) / rate))])
                sock.sendall(pcm); packet = sock.recv(32768)
                assert 0 < len(packet) < 8192, len(packet)
                encoded += bytes([0x56, 0xe0 | (len(packet) >> 8), len(packet) & 255]) + packet
        time.sleep(.1); assert call('CodecCleanUp').returncode == 0
        latm = a.output / f'aac-{rate}.loas'; latm.write_bytes(encoded)
        decoded = a.output / f'aac-{rate}.pcm'
        command = [str(a.ffmpeg), '-v', 'error', '-y', '-f', 'loas', '-i', str(latm),
            '-f', 's16le', '-acodec', 'pcm_s16le', str(decoded)]
        run = subprocess.run(command, env=env, capture_output=True, text=True, timeout=10)
        assert run.returncode == 0, run.stderr
        samples = array.array('h', decoded.read_bytes())
        rms = math.sqrt(sum(v*v for v in samples) / len(samples))
        assert len(samples) >= 28 * 1024 * 2 and rms > 4000, (len(samples), rms)
        # Ignore AAC priming and compare against the submitted 440 Hz tone.
        # Phase-independent correlation rejects silence/filler and wrong rates.
        channel = samples[4096 * 2::2]
        real = sum(value * math.cos(2 * math.pi * 440 * index / rate)
                   for index, value in enumerate(channel))
        imag = sum(value * math.sin(2 * math.pi * 440 * index / rate)
                   for index, value in enumerate(channel))
        tone_energy_fraction = 2 * (real * real + imag * imag) / (
            len(channel) * sum(value * value for value in channel))
        assert tone_energy_fraction > .85, tone_energy_fraction
        results[str(rate)] = {'frames': 30, 'decodedSamples': len(samples), 'rms': rms,
                             'toneEnergyFraction': tone_energy_fraction}
    for invalid in ((44100, 8, 2), (96000, 16, 2), (44100, 16, 1)):
        assert init(*invalid).returncode != 0, invalid
    results['invalidConfigurationsRejected'] = True
    # A truncated frame must terminate only that client, not corrupt encoder state.
    path, _ = parse_response(init(44100))
    with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as sock:
        sock.settimeout(3); sock.connect(path); sock.sendall(b'bad'); assert sock.recv(32768) == b''
    time.sleep(.1); assert call('CodecCleanUp').returncode == 0
    assert mmc.poll() is None
    results['malformedFrameRejected'] = True
    # Simulate repeated A2DP/HFP profile turnover: every encoder must release
    # its socket, codec context, and worker slot for the following session.
    for cycle in range(12):
        path, count = parse_response(init(44100))
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as sock:
            sock.settimeout(3); sock.connect(path)
            sock.sendall(bytes(count * 4)); assert sock.recv(32768)
        time.sleep(.03); assert call('CodecCleanUp').returncode == 0
    results['repeatedEncoderSessions'] = 12
    assert mmc.poll() is None
    (a.output / 'summary.json').write_text(json.dumps(results, indent=2) + '\n')
    print(json.dumps(results, indent=2))
finally:
    for process in reversed(children):
        if process.poll() is None: process.terminate()
    for process in reversed(children):
        try: process.wait(timeout=3)
        except subprocess.TimeoutExpired: process.kill(); process.wait()
    for log in logs: log.close()
