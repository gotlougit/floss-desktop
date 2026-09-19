#!/usr/bin/env python3
"""Actual pw-floss HFP/A2DP PCM transport against a private mock, never hardware."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--pipewire', required=True, type=Path)
p.add_argument('--wireplumber', required=True, type=Path)
p.add_argument('--mock', required=True, type=Path)
p.add_argument('--bridge', required=True, type=Path, help='Standalone pw-floss binary')
p.add_argument('--output', required=True, type=Path)
p.add_argument('--inside', action='store_true', help=argparse.SUPPRESS)
a = p.parse_args()
if not a.inside:
    a.output.mkdir(parents=True, exist_ok=True)
    (a.output / 'summary.json').unlink(missing_ok=True)
    command = [shutil.which('bwrap'), '--die-with-parent', '--unshare-all', '--cap-drop', 'ALL',
        '--tmpfs', '/', '--ro-bind', '/nix/store', '/nix/store', '--ro-bind', '/etc', '/etc',
        '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp', '--tmpfs', '/run', '--tmpfs', '/var',
        '--dir', '/sys', '--ro-bind', str(Path(__file__).resolve()), '/smoke.py',
        '--ro-bind', str(a.mock.resolve()), '/mock',
        '--ro-bind', str(a.bridge.resolve()), '/bridge', '--bind', str(a.output.resolve()), '/report',
        '--setenv', 'SMOKE_BRIDGE_ORIGIN', str(a.bridge.resolve()),
        '--setenv', 'SMOKE_DBUS', str(Path(shutil.which('dbus-daemon')).resolve()),
        str(Path(sys.executable).resolve()), '/smoke.py', '--inside', '--pipewire', str(a.pipewire.resolve()),
        '--wireplumber', str(a.wireplumber.resolve()), '--mock', '/mock', '--bridge', '/bridge', '--output', '/report']
    sys.exit(subprocess.call(command))

for name in ('/tmp/runtime', '/tmp/config', '/tmp/state', '/tmp/cache', '/tmp/data', '/var/run/bluetooth/audio'):
    Path(name).mkdir(parents=True, mode=0o700, exist_ok=True)
env = {k: v for k, v in os.environ.items() if not k.startswith(('PIPEWIRE_', 'WIREPLUMBER_', 'SPA_', 'DBUS_'))}
env.update(XDG_RUNTIME_DIR='/tmp/runtime', XDG_CONFIG_HOME='/tmp/config', XDG_STATE_HOME='/tmp/state',
    XDG_CACHE_HOME='/tmp/cache', XDG_DATA_HOME='/tmp/data', HOME='/tmp',
    PIPEWIRE_RUNTIME_DIR='/tmp/runtime', PIPEWIRE_REMOTE='pipewire-0',
    PIPEWIRE_CONFIG_DIR=str(a.pipewire / 'share/pipewire'),
    WIREPLUMBER_CONFIG_DIR=str(a.wireplumber / 'share/wireplumber'),
    SPA_PLUGIN_DIR=str(a.pipewire / 'lib/spa-0.2'))
Path('/tmp/bus.conf').write_text('''<busconfig><type>session</type><listen>unix:path=/tmp/private-bus</listen>
<policy context="default"><allow own="*"/><allow send_destination="*"/><allow receive_sender="*"/></policy></busconfig>''')
env['DBUS_SYSTEM_BUS_ADDRESS'] = env['DBUS_SESSION_BUS_ADDRESS'] = 'unix:path=/tmp/private-bus'
processes, logs, checks = [], [], []
def start(name, command):
    log = (a.output / (name + '.log')).open('w'); logs.append(log)
    child = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
    processes.append(child); return child

def stop(child):
    if child.poll() is None:
        child.terminate()
        try: child.wait(timeout=5)
        except subprocess.TimeoutExpired: child.kill(); child.wait()

def wait_for(predicate, message):
    deadline = time.monotonic() + 8
    while not predicate():
        assert time.monotonic() < deadline, message
        time.sleep(.05)

def graph():
    return json.loads(subprocess.check_output([str(a.pipewire / 'bin/pw-dump')], env=env, timeout=5))

def nodes():
    return [o.get('info', {}).get('props', {}) for o in graph()
        if o.get('type') == 'PipeWire:Interface:Node' and
        o.get('info', {}).get('props', {}).get('device.api') == 'floss']
bridge_command = [str(a.bridge), '--profile', 'hfp', '--device', 'AA:BB:CC:DD:EE:FF']
try:
    start('bus', [os.environ['SMOKE_DBUS'], '--nofork', '--config-file=/tmp/bus.conf'])
    wait_for(lambda: Path('/tmp/private-bus').exists(), 'bus missing')
    start('pipewire', [str(a.pipewire / 'bin/pipewire')])
    wait_for(lambda: Path('/tmp/runtime/pipewire-0').exists(), 'PipeWire socket missing')
    wp = start('wireplumber', [str(a.wireplumber / 'bin/wireplumber'), '--profile=policy'])
    time.sleep(1); assert wp.poll() is None
    for mode in ('normal', 'bad-format', 'generation-change', 'disconnect', 'owner-loss', 'a2dp', 'a2dp-counter-reset'):
        mock = start('mock-' + mode, [str(a.mock), mode])
        wait_for(lambda: 'ready' in (a.output / ('mock-' + mode + '.log')).read_text(), 'mock missing')
        command = bridge_command if not mode.startswith('a2dp') else [str(a.bridge),
            '--profile', 'a2dp', '--device', 'AA:BB:CC:DD:EE:FF']
        bridge = start('bridge-' + mode, command)
        if mode == 'normal':
            wait_for(lambda: len(nodes()) == 2, 'actual bridge sink/source missing')
            props = nodes()
            assert {n['media.class'] for n in props} == {'Audio/Sink', 'Audio/Source'}
            (a.output / 'actual-bridge-nodes.json').write_text(json.dumps(props, indent=2))
            # The second bridge must reject ownership without disturbing the first.
            duplicate = subprocess.run(bridge_command, env=env, capture_output=True, text=True, timeout=5)
            (a.output / 'duplicate.log').write_text(duplicate.stdout + duplicate.stderr)
            assert duplicate.returncode == 1 and bridge.poll() is None
            playback = next(n['node.name'] for n in props if n['media.class'] == 'Audio/Sink')
            capture = next(n['node.name'] for n in props if n['media.class'] == 'Audio/Source')
            Path('/tmp/play.raw').write_bytes(b''.join(struct.pack('<h', 8000 if i % 40 < 20 else -8000) for i in range(16000 * 12)))
            record = start('record', [str(a.pipewire / 'bin/pw-cat'), '--record', '--raw', '--rate=16000', '--channels=1', '--format=s16', '--latency=20ms', '--target=' + capture, '/tmp/capture.raw'])
            play = start('play', [str(a.pipewire / 'bin/pw-cat'), '--playback', '--raw', '--rate=16000', '--channels=1', '--format=s16', '--latency=20ms', '--target=' + playback, '/tmp/play.raw'])
            time.sleep(10)
            assert bridge.poll() is None, 'bridge failed while streaming'
            stop(play); stop(record)
            captured = Path('/tmp/capture.raw').read_bytes()
            assert len(captured) >= 16000 and any(captured), ('capture PCM absent', len(captured))
            samples = struct.unpack('<' + 'h' * (len(captured) // 2), captured)
            middle = samples[len(samples) // 4:3 * len(samples) // 4]
            crossings, polarity = 0, 0
            for sample in middle:
                next_polarity = 1 if sample > 6000 else -1 if sample < -6000 else polarity
                if polarity and next_polarity != polarity: crossings += 1
                polarity = next_polarity
            frequency = crossings * 16000 / len(middle) / 2
            rms = (sum(x*x for x in middle) / len(middle)) ** .5
            assert 475 < frequency < 525 and 9000 < rms < 14000, (frequency, rms)
            stop(bridge); assert bridge.returncode == 0
            checks.append({'case': mode, 'captureBytes': len(captured), 'captureToneHz': frequency, 'captureRms': rms, 'duplicateRejected': True})
        elif mode == 'a2dp':
            wait_for(lambda: len(nodes()) == 1, 'A2DP sink missing')
            props = nodes(); assert props[0]['media.class'] == 'Audio/Sink'
            Path('/tmp/a2dp.raw').write_bytes(b''.join(struct.pack('<hh', 8000 if i % 120 < 60 else -8000, 8000 if i % 120 < 60 else -8000) for i in range(48000 * 6)))
            play = start('a2dp-play', [str(a.pipewire / 'bin/pw-cat'), '--playback', '--raw', '--rate=48000', '--channels=2', '--format=s16', '--latency=20ms', '--target=' + props[0]['node.name'], '/tmp/a2dp.raw'])
            time.sleep(4); assert bridge.poll() is None, 'A2DP bridge failed while streaming'
            stop(play); stop(bridge); assert bridge.returncode == 0
            checks.append({'case': mode, 'exitCode': bridge.returncode})
        else:
            bridge.wait(timeout=10)
            assert bridge.returncode == 1, (mode, bridge.returncode)
            error = (a.output / ('bridge-' + mode + '.log')).read_text()
            expected = {'bad-format': 'missing or unsupported negotiated PCM configuration', 'generation-change': 'transport changed while opening PCM', 'disconnect': 'PCM transport disconnected', 'owner-loss': 'Floss owner changed', 'a2dp-counter-reset': 'transport counter reset'}[mode]
            assert expected in error, error
            checks.append({'case': mode, 'exitCode': bridge.returncode})
        stop(mock); assert mock.returncode == 0, (mode, 'mock protocol assertion failed', mock.returncode)
        report = (a.output / ('mock-' + mode + '.log')).read_text()
        assert 'lease-stopped' in report, (mode, report)
        if mode in ('normal', 'a2dp'):
            nonzero = int(re.search(r'nonzero=(\d+)', report)[1])
            assert nonzero > 16000, report
            checks[-1]['playbackNonzeroBytes'] = nonzero
        wait_for(lambda: not nodes(), 'bridge nodes leaked')
finally:
    for child in reversed(processes): stop(child)
    for log in logs: log.close()
(a.output / 'summary.json').write_text(json.dumps({'pipewirePackage': str(a.pipewire), 'wireplumberPackage': str(a.wireplumber), 'bridgeExecutable': os.environ.get('SMOKE_BRIDGE_ORIGIN', str(a.bridge)), 'bridgeSha256': hashlib.sha256(a.bridge.read_bytes()).hexdigest(), 'checks': checks, 'limitations': 'Mock HFP/A2DP transports only: no controller, radio, codecs or physical headset.'}, indent=2) + '\n')
print(json.dumps(checks, indent=2))
