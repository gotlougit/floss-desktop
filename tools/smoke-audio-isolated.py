#!/usr/bin/env python3
"""Unprivileged audio/policy smoke checks on private sockets; no hardware."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--pipewire', required=True, type=Path)
parser.add_argument('--bridge', required=True, type=Path)
parser.add_argument('--wireplumber', required=True, type=Path)
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
processes = []
logs = []
results = []
with tempfile.TemporaryDirectory(prefix='floss-audio-smoke-') as temporary:
    base = Path(temporary)
    for name in ('runtime', 'config', 'state', 'data', 'cache'):
        (base / name).mkdir(mode=0o700)
    env = os.environ.copy()
    for key in list(env):
        if key.startswith(('PIPEWIRE_', 'WIREPLUMBER_', 'SPA_')) or key in (
                'DBUS_SESSION_BUS_ADDRESS', 'DBUS_SYSTEM_BUS_ADDRESS'):
            env.pop(key, None)
    env.update({
        'XDG_RUNTIME_DIR': str(base / 'runtime'),
        'XDG_CONFIG_HOME': str(base / 'config'),
        'XDG_STATE_HOME': str(base / 'state'),
        'XDG_DATA_HOME': str(base / 'data'),
        'XDG_CACHE_HOME': str(base / 'cache'),
        'PIPEWIRE_RUNTIME_DIR': str(base / 'runtime'),
        'PIPEWIRE_REMOTE': 'pipewire-0',
        'PIPEWIRE_CONFIG_DIR': str(args.pipewire / 'share/pipewire'),
        'WIREPLUMBER_CONFIG_DIR': str(args.wireplumber / 'share/wireplumber'),
        'SPA_PLUGIN_DIR': str(args.pipewire / 'lib/spa-0.2'),
    })
    # No service directories: this bus cannot activate host services.
    conf = base / 'bus.conf'
    conf.write_text(f'''<busconfig><type>session</type>
      <listen>unix:tmpdir={base / 'runtime'}</listen>
      <policy context="default"><allow send_destination="*"/>
      <allow receive_sender="*"/><allow own="*"/></policy></busconfig>''')

    def start(name, command):
        log = (args.output / (name + '.log')).open('w')
        logs.append(log)
        process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
        processes.append(process)
        return process

    def run(name, command, expected=0):
        result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=15)
        (args.output / (name + '.log')).write_text(result.stdout + result.stderr)
        assert result.returncode == expected, (name, result.returncode, result.stderr)
        results.append({'check': name, 'exitCode': result.returncode})
        return result

    try:
        bus = subprocess.Popen([shutil.which('dbus-daemon'), '--nofork',
            '--config-file=' + str(conf), '--print-address=1'], env=env,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        processes.append(bus)
        address = bus.stdout.readline().strip()
        assert address.startswith('unix:'), 'private D-Bus failed to start: ' + bus.stderr.read()
        env['DBUS_SESSION_BUS_ADDRESS'] = address
        env['DBUS_SYSTEM_BUS_ADDRESS'] = address
        pipewire = start('pipewire', [str(args.pipewire / 'bin/pipewire')])
        deadline = time.monotonic() + 10
        while not (base / 'runtime/pipewire-0').exists():
            assert pipewire.poll() is None, 'PipeWire exited before socket creation'
            assert time.monotonic() < deadline, 'PipeWire socket timeout'
            time.sleep(0.1)
        # The policy-only profile deliberately loads no audio/video hardware monitors.
        wireplumber = start('wireplumber', [str(args.wireplumber / 'bin/wireplumber'),
                                            '--profile=policy'])
        time.sleep(2)
        assert wireplumber.poll() is None, 'WirePlumber failed to initialize'
        run('initial-status', [str(args.wireplumber / 'bin/wpctl'), 'status'])
        run('bridge-help', [str(args.bridge), '--help'])
        run('bridge-reject-hfp-rate', [str(args.bridge),
            '--profile', 'hfp', '--device', 'AA:BB:CC:DD:EE:FF', '--pcm-rate', '48000'], 64)
        run('bridge-reject-manual-pcm-rate', [str(args.bridge),
            '--profile', 'a2dp', '--device', 'AA:BB:CC:DD:EE:FF', '--pcm-rate', '48000'], 64)
        absent = run('bridge-no-daemon', [str(args.bridge),
            '--profile', 'hfp', '--device', 'AA:BB:CC:DD:EE:FF'], 1)
        assert 'org.chromium.bluetooth' in absent.stderr and 'no such name' in absent.stderr.lower(), absent.stderr
        loop = start('synthetic-loopback', [str(args.pipewire / 'bin/pw-loopback'),
            '--capture-props=media.class=Audio/Sink node.name=floss_smoke_output device.api=floss api.floss.profile=hfp',
            '--playback-props=media.class=Audio/Source node.name=floss_smoke_input device.api=floss api.floss.profile=hfp'])
        time.sleep(2)
        assert loop.poll() is None, 'synthetic loopback failed'
        dumped = run('graph', [str(args.pipewire / 'bin/pw-dump')])
        objects = json.loads(dumped.stdout)
        props = [obj.get('info', {}).get('props', {}) for obj in objects]
        synthetic = [p for p in props if p.get('node.name', '').startswith('floss_smoke_')]
        assert {p.get('media.class') for p in synthetic} == {'Audio/Sink', 'Audio/Source'}, synthetic
        assert not any(p.get('device.api') in ('alsa', 'v4l2', 'libcamera', 'bluez5') for p in props), 'unexpected hardware monitor'
        status = run('policy-status', [str(args.wireplumber / 'bin/wpctl'), 'status'])
        assert 'floss_smoke_' in status.stdout, status.stdout
        assert pipewire.poll() is None and wireplumber.poll() is None
        results.append({'check': 'synthetic-floss-sink-source-recognized', 'passed': True})
    finally:
        for process in reversed(processes):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        for log in logs:
            log.close()
(args.output / 'summary.json').write_text(json.dumps({
    'pipewirePackage': str(args.pipewire),
    'wireplumberPackage': str(args.wireplumber),
    'checks': results,
    'limitations': 'Synthetic nodes and absent-daemon handling only; no Floss daemon, PCM transport, codecs, controller, or headset exercised.'
}, indent=2) + '\n')
print(json.dumps(results, indent=2))
