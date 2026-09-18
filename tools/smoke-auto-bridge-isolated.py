#!/usr/bin/env python3
"""Automatic bridge/WirePlumber lifecycle matrix with scripted devices, never hardware."""
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
p.add_argument('--bridge', type=Path, help='Optional incremental bridge binary')
p.add_argument('--output', required=True, type=Path)
p.add_argument('--cases', default='sbc,aac,speaker,mono,cvsd,msbc,delayed,hfp-only,denied,lifecycle,defaults')
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
        '--ro-bind', str((a.bridge or a.pipewire / 'bin/pw-floss').resolve()), '/bridge', '--bind', str(a.output.resolve()), '/report',
        '--setenv', 'SMOKE_BRIDGE_ORIGIN', str((a.bridge or a.pipewire / 'bin/pw-floss').resolve()),
        '--setenv', 'SMOKE_DBUS', str(Path(shutil.which('dbus-daemon')).resolve()),
        str(Path(sys.executable).resolve()), '/smoke.py', '--inside', '--pipewire', str(a.pipewire.resolve()),
        '--wireplumber', str(a.wireplumber.resolve()), '--mock', '/mock', '--bridge', '/bridge', '--cases', a.cases, '--output', '/report']
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
bridge_command = [str(a.bridge or a.pipewire / 'bin/pw-floss'), '--auto', '--adapter', '0']

def report(name):
    return (a.output / (name + '.log')).read_text()

def starts(name, profile):
    return report(name).count('start profile=' + profile)

active_mock_name = None
def current_profile():
    events = re.findall(r'(start|stop) profile=(a2dp|hfp)', report(active_mock_name))
    return events[-1][1] if events and events[-1][0] == 'start' else None

def control(value):
    Path('/tmp/mock-control').write_text(value + '\n')

def node_ids():
    return {o['info']['props']['node.name']: o['id'] for o in graph()
        if o.get('type') == 'PipeWire:Interface:Node' and
        o.get('info', {}).get('props', {}).get('device.api') == 'floss'}

def record(name, target, passive=False):
    command = [str(a.pipewire / 'bin/pw-cat'), '--record', '--raw', '--rate=16000', '--channels=1',
        '--format=s16', '--latency=20ms']
    if target: command += ['--target=' + target]
    if passive: command += ['--properties=node.passive=true stream.monitor=true']
    return start(name, command + ['/tmp/' + name + '.raw'])

def playback(name, target):
    command = [str(a.pipewire / 'bin/pw-cat'), '--playback', '--raw', '--rate=48000',
        '--channels=2', '--format=s16', '--latency=20ms']
    if target: command += ['--target=' + target]
    return start(name, command + ['/tmp/music.raw'])

def begin(kind, label):
    global active_mock_name
    active_mock_name = 'mock-' + label
    control('online')
    mock = start('mock-' + label, [str(a.mock), kind])
    wait_for(lambda: 'ready' in report('mock-' + label), 'mock not ready')
    bridge = start('bridge-' + label, bridge_command)
    wait_for(lambda: len(nodes()) == (1 if kind in ('speaker', 'mono') else 2), 'automatic nodes missing: ' + label)
    wait_for(lambda: current_profile() == ('hfp' if kind == 'hfp-only' else 'a2dp'), 'initial profile missing')
    return mock, bridge

def finish(mock, bridge):
    stop(bridge); assert bridge.returncode == 0, report('wireplumber')
    stop(mock); assert mock.returncode == 0, 'mock protocol violation'
    wait_for(lambda: not nodes(), 'nodes leaked after exit')

try:
    start('bus', [os.environ['SMOKE_DBUS'], '--nofork', '--config-file=/tmp/bus.conf'])
    wait_for(lambda: Path('/tmp/private-bus').exists(), 'bus missing')
    start('pipewire', [str(a.pipewire / 'bin/pipewire')])
    wait_for(lambda: Path('/tmp/runtime/pipewire-0').exists(), 'PipeWire socket missing')
    wp = start('wireplumber', [str(a.wireplumber / 'bin/wireplumber'), '--profile=policy'])
    time.sleep(1); assert wp.poll() is None
    # Synthetic independent microphone, not a host ALSA monitor.
    builtin = start('builtin-mic', [str(a.pipewire / 'bin/pw-loopback'),
        '--capture-props=media.class=Audio/Sink node.name=test_builtin_feeder node.pause-on-idle=true priority.session=1009',
        '--playback-props=media.class=Audio/Source node.name=test_builtin_mic node.pause-on-idle=true priority.session=2009'])
    Path('/tmp/music.raw').write_bytes(b''.join(struct.pack('<hh', 8000 if i % 120 < 60 else -8000,
        8000 if i % 120 < 60 else -8000) for i in range(48000 * 40)))
    for kind in (item for item in a.cases.split(',') if item not in ('lifecycle', 'denied', 'hfp-only', 'defaults')):
        mock, bridge = begin(kind, kind)
        mock_name = 'mock-' + kind
        props = nodes()
        sink = next(n['node.name'] for n in props if n['media.class'] == 'Audio/Sink')
        ids = node_ids()
        play = playback(kind + '-play', sink)
        time.sleep(1)
        assert bridge.poll() is None and current_profile() == 'a2dp'
        other = record(kind + '-builtin-record', 'test_builtin_mic')
        time.sleep(1.5)
        assert starts(mock_name, 'hfp') == 0, 'builtin microphone caused HFP switch'
        stop(other)
        case = {'kind': kind, 'builtinMicDoesNotSwitch': True, 'initialNodeCount': len(props)}
        if kind not in ('speaker', 'mono'):
            source = next(n['node.name'] for n in props if n['media.class'] == 'Audio/Source')
            meter = record(kind + '-passive-meter', source, passive=True)
            time.sleep(1)
            assert starts(mock_name, 'hfp') == 0, 'passive meter caused HFP switch'
            stop(meter)
            capture = record(kind + '-headset-record', source)
            wait_for(lambda: starts(mock_name, 'hfp') == 1, 'recording did not request HFP')
            wait_for(lambda: current_profile() == 'hfp', 'HFP profile not published')
            time.sleep(2)
            assert node_ids() == ids, 'node identities changed entering HFP'
            assert bridge.poll() is None and capture.poll() is None and play.poll() is None
            stop(capture)
            # Brief consumer restart must not release and reacquire transport.
            time.sleep(.25)
            capture2 = record(kind + '-headset-record2', source)
            time.sleep(.75)
            assert starts(mock_name, 'hfp') == 1, 'rapid recording restart caused churn'
            stop(capture2)
            wait_for(lambda: current_profile() == 'a2dp', 'idle microphone did not return A2DP')
            assert node_ids() == ids, 'node identities changed leaving HFP'
            data = Path('/tmp/' + kind + '-headset-record.raw').read_bytes()
            assert len(data) >= 16000 and any(data), ('microphone PCM absent', kind, len(data))
            samples = struct.unpack('<' + 'h' * (len(data) // 2), data)
            middle = samples[len(samples) // 4:3 * len(samples) // 4]
            crossings, polarity = 0, 0
            for sample in middle:
                next_polarity = 1 if sample > 6000 else -1 if sample < -6000 else polarity
                if polarity and next_polarity != polarity: crossings += 1
                polarity = next_polarity
            frequency = crossings * 16000 / len(middle) / 2
            assert 475 < frequency < 525, ('microphone rate conversion corrupted tone', kind, frequency)
            case['captureToneHz'] = frequency
            case.update(headsetMicSwitches=True, passiveMeterDoesNotSwitch=True,
                recordingRestartDebounced=True, returnsA2dp=True, stableNodeIds=True, capturedBytes=len(data))
        else:
            assert not any(n['media.class'] == 'Audio/Source' for n in nodes())
            assert starts(mock_name, 'hfp') == 0
            case['noPhantomMicrophone'] = True
        stop(play)
        finish(mock, bridge)
        text = report(mock_name)
        assert int(re.search(r'a2dp_nonzero=(\d+)', text)[1]) > 4000, text
        case['a2dpNonzeroBytes'] = int(re.search(r'a2dp_nonzero=(\d+)', text)[1])
        if kind == 'mono':
            data = Path('/tmp/mock-mono.raw').read_bytes()
            samples = struct.unpack('<' + 'h' * (len(data) // 2), data)
            middle = samples[len(samples) // 4:3 * len(samples) // 4]
            crossings, polarity = 0, 0
            for sample in middle:
                next_polarity = 1 if sample > 3000 else -1 if sample < -3000 else polarity
                if polarity and next_polarity != polarity: crossings += 1
                polarity = next_polarity
            frequency = crossings * 48000 / len(middle) / 2
            assert 380 < frequency < 420, ('mono playback conversion corrupted tone', frequency)
            case['a2dpToneHz'] = frequency
        if kind not in ('speaker', 'mono'):
            case['hfpNonzeroBytes'] = int(re.search(r'hfp_nonzero=(\d+)', text)[1])
            assert case['hfpNonzeroBytes'] > 4000, ('HFP playback stayed silent', text)
        checks.append(case)
    if 'hfp-only' in a.cases.split(','):
        mock, bridge = begin('hfp-only', 'hfp-only')
        sink = next(n['node.name'] for n in nodes() if n['media.class'] == 'Audio/Sink')
        source = next(n['node.name'] for n in nodes() if n['media.class'] == 'Audio/Source')
        play = playback('voice-only-play', sink)
        capture = record('voice-only-record', source)
        time.sleep(2)
        stop(capture); time.sleep(3)
        assert current_profile() == 'hfp' and starts('mock-hfp-only', 'a2dp') == 0
        stop(play); finish(mock, bridge)
        text = report('mock-hfp-only')
        assert int(re.search(r'hfp_nonzero=(\d+)', text)[1]) > 4000
        checks.append({'case': 'hfp-only', 'remainsInVoiceProfile': True, 'noA2dpRequested': True})
    if 'denied' in a.cases.split(','):
        mock, bridge = begin('denied', 'denied')
        source = next(n['node.name'] for n in nodes() if n['media.class'] == 'Audio/Source')
        ids = node_ids()
        capture = record('denied-record', source)
        wait_for(lambda: 'reserve profile=hfp token=0' in report('mock-denied'), 'HFP request not attempted')
        wait_for(lambda: current_profile() == 'a2dp' and starts('mock-denied', 'a2dp') == 2,
            'denied HFP did not restore A2DP after bounded admission retries')
        attempts = report('mock-denied').count('reserve profile=hfp token=0')
        time.sleep(2)
        assert bridge.poll() is None and current_profile() == 'a2dp' and node_ids() == ids
        assert starts('mock-denied', 'hfp') == 0
        assert report('mock-denied').count('reserve profile=hfp token=0') == attempts, 'denied HFP retry churn'
        stop(capture); finish(mock, bridge)
        checks.append({'case': 'denied', 'returnsA2dp': True, 'noRetryChurn': True, 'stableNodeIds': True})
    if 'lifecycle' in a.cases.split(','):
        # Lifecycle faults use the same long-running bridge process.
        mock, bridge = begin('msbc', 'lifecycle')
        sink = next(n['node.name'] for n in nodes() if n['media.class'] == 'Audio/Sink')
        source = next(n['node.name'] for n in nodes() if n['media.class'] == 'Audio/Source')
        play = playback('disconnect-play', sink)
        ids = node_ids(); control('second'); time.sleep(2)
        assert node_ids() == ids and bridge.poll() is None, 'second device stole active audio'
        control('online')
        time.sleep(.5); control('offline')
        wait_for(lambda: not nodes(), 'playback disconnect retained nodes')
        assert bridge.poll() is None, 'automatic bridge exited on device disconnect'
        stop(play); control('online')
        wait_for(lambda: len(nodes()) == 2, 'device reconnect not detected')
        capture = record('disconnect-call', source)
        wait_for(lambda: current_profile() == 'hfp', 'call did not start')
        control('offline'); wait_for(lambda: not nodes(), 'call disconnect retained nodes')
        assert bridge.poll() is None
        stop(capture); control('online')
        wait_for(lambda: len(nodes()) == 2 and current_profile() == 'a2dp', 'call reconnect failed')
        control('speaker')
        wait_for(lambda: len(nodes()) == 1, 'removed HFP capability retained microphone')
        control('headset')
        wait_for(lambda: len(nodes()) == 2, 'added HFP capability missing microphone')
        stop(mock); assert mock.returncode == 0
        wait_for(lambda: not nodes(), 'daemon loss retained nodes')
        assert bridge.poll() is None, 'automatic bridge exited on daemon loss'
        active_mock_name = 'mock-restarted'
        mock = start('mock-restarted', [str(a.mock), 'msbc'])
        wait_for(lambda: 'ready' in report('mock-restarted'), 'restarted mock missing')
        wait_for(lambda: len(nodes()) == 2 and current_profile() == 'a2dp', 'daemon restart recovery failed')
        finish(mock, bridge)
        checks.append({'case': 'lifecycle', 'disconnectDuringPlayback': True, 'disconnectDuringCall': True,
            'capabilityRemovalAndReturn': True, 'daemonRestart': True, 'secondDeviceDoesNotStealActiveAudio': True})
    if 'defaults' in a.cases.split(','):
        def default_node(kind):
            for obj in graph():
                for item in obj.get('metadata', []):
                    if item.get('key') == 'default.audio.' + kind:
                        return item['value']['name']
        mock, bridge = begin('msbc', 'defaults')
        source = next(n['node.name'] for n in nodes() if n['media.class'] == 'Audio/Source')
        wait_for(lambda: default_node('source') == source, 'new headset not selected as default microphone')
        sink = next(n['node.name'] for n in nodes() if n['media.class'] == 'Audio/Sink')
        wait_for(lambda: default_node('sink') == sink, 'new headset not selected as default playback')
        default_play = playback('default-play', None)
        capture = record('default-headset-record', None)
        wait_for(lambda: current_profile() == 'hfp', 'untargeted desktop recording did not use headset')
        time.sleep(.5); stop(capture)
        wait_for(lambda: current_profile() == 'a2dp', 'untargeted recorder idle did not restore A2DP')
        builtin_id = next(o['id'] for o in graph() if o.get('info', {}).get('props', {}).get('node.name') == 'test_builtin_mic')
        subprocess.run([str(a.wireplumber / 'bin/wpctl'), 'set-default', str(builtin_id)], env=env, check=True)
        wait_for(lambda: default_node('source') == 'test_builtin_mic', 'explicit builtin preference not applied')
        count = starts('mock-defaults', 'hfp')
        capture = record('default-builtin-record', None)
        time.sleep(1)
        assert starts('mock-defaults', 'hfp') == count, 'explicit builtin recording switched headset'
        stop(capture); control('offline')
        wait_for(lambda: not nodes(), 'default test disconnect failed'); control('online')
        wait_for(lambda: len(nodes()) == 2, 'default test reconnect failed')
        assert default_node('source') == 'test_builtin_mic', 'reconnect overrode explicit user default'
        stop(default_play); finish(mock, bridge)
        checks.append({'case': 'defaults', 'untargetedPlaybackUsesHeadset': True, 'untargetedRecordingUsesHeadset': True,
            'explicitBuiltinPreferenceHonored': True, 'reconnectPreservesUserPreference': True})

finally:
    for child in reversed(processes): stop(child)
    for log in logs: log.close()
(a.output / 'summary.json').write_text(json.dumps({'pipewirePackage': str(a.pipewire),
    'wireplumberPackage': str(a.wireplumber),
    'bridgeExecutable': os.environ.get('SMOKE_BRIDGE_ORIGIN', str(a.bridge or a.pipewire / 'bin/pw-floss')),
    'bridgeSha256': hashlib.sha256((a.bridge or a.pipewire / 'bin/pw-floss').read_bytes()).hexdigest(),
    'checks': checks,
    'limitations': 'Scripted capability/PCM peers only. Advertised AAC is not proof of actual AAC negotiation or encoding; no controller/radio/headset.'}, indent=2) + '\n')
print(json.dumps(checks, indent=2))
