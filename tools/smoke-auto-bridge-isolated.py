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
p.add_argument('--bridge', required=True, type=Path, help='Standalone pw-floss binary')
p.add_argument('--peak-meter', type=Path, help='Compiled pulse-peak-meter.c fixture')
p.add_argument('--vlc', type=Path, help='Existing VLC executable in /nix/store for corked startup regression')
p.add_argument('--pulse-tools', type=Path, help='Existing PulseAudio bin directory for pulse-pause regression')
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
        '--ro-bind', str(a.bridge.resolve()), '/bridge', '--bind', str(a.output.resolve()), '/report',
        '--setenv', 'SMOKE_BRIDGE_ORIGIN', str(a.bridge.resolve()),
        '--setenv', 'SMOKE_DBUS', str(Path(shutil.which('dbus-daemon')).resolve()),
        str(Path(sys.executable).resolve()), '/smoke.py', '--inside', '--pipewire', str(a.pipewire.resolve()),
        '--wireplumber', str(a.wireplumber.resolve()), '--mock', '/mock', '--bridge', '/bridge', '--cases', a.cases, '--output', '/report']
    if a.pulse_tools:
        command += ['--pulse-tools', str(a.pulse_tools.resolve())]
    if a.peak_meter:
        mount_index = command.index('--setenv')
        command[mount_index:mount_index] = ['--ro-bind', str(a.peak_meter.resolve()), '/peak-meter']
        command += ['--peak-meter', '/peak-meter']
    if a.vlc:
        command += ['--vlc', str(a.vlc.resolve())]
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
bridge_command = [str(a.bridge), '--auto', '--adapter', '0']

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

def node_ports(node_id):
    return [o.get('info', {}).get('props', {}) for o in graph()
        if o.get('type') == 'PipeWire:Interface:Port' and
        o.get('info', {}).get('props', {}).get('node.id') == node_id]

def volume(node_id):
    output = subprocess.check_output([str(a.wireplumber / 'bin/wpctl'), 'get-volume', str(node_id)],
        env=env, text=True, timeout=5)
    match = re.search(r'Volume:\s+([0-9.]+)', output)
    assert match, ('source volume unavailable', output)
    return float(match[1])

def pcm_metrics(data, rate):
    samples = struct.unpack('<' + 'h' * (len(data) // 2), data)
    middle = samples[len(samples) // 4:3 * len(samples) // 4]
    assert middle, 'microphone PCM contained no complete samples'
    crossings, polarity = 0, 0
    for sample in middle:
        next_polarity = 1 if sample > 6000 else -1 if sample < -6000 else polarity
        if polarity and next_polarity != polarity: crossings += 1
        polarity = next_polarity
    nonzero = sum(sample != 0 for sample in middle)
    positive = sum(sample > 1000 for sample in middle)
    negative = sum(sample < -1000 for sample in middle)
    metrics = {
        'toneHz': crossings * rate / len(middle) / 2,
        'peak': max(abs(sample) for sample in middle),
        'meanAbsolute': sum(abs(sample) for sample in middle) / len(middle),
        'dcOffset': sum(middle) / len(middle),
        'nonzeroFraction': nonzero / len(middle),
        'positiveFraction': positive / len(middle),
        'negativeFraction': negative / len(middle),
    }
    assert metrics['peak'] > 7000, ('microphone level too low', metrics)
    assert metrics['meanAbsolute'] > 3000, ('microphone signal energy too low', metrics)
    assert abs(metrics['dcOffset']) < 1500, ('microphone has excessive DC offset', metrics)
    assert metrics['nonzeroFraction'] > .8, ('microphone mostly silent', metrics)
    assert metrics['positiveFraction'] > .2 and metrics['negativeFraction'] > .2, \
        ('microphone lost one polarity', metrics)
    return metrics

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
    if 'peak-meter' in a.cases.split(','):
        assert a.peak_meter, '--peak-meter is required'
        env['PULSE_SERVER'] = 'unix:/tmp/runtime/pulse/native'
        start('pulse-meter', [str(a.pipewire / 'bin/pipewire-pulse')])
        wait_for(lambda: Path('/tmp/runtime/pulse/native').exists(), 'Pulse socket missing')
        mock, bridge = begin('cvsd', 'peak-meter')
        source = next(n['node.name'] for n in nodes() if n['media.class'] == 'Audio/Source')
        meter = start('peak-meter', [str(a.peak_meter), source])
        time.sleep(2)
        assert meter.poll() is None, report('peak-meter')
        (a.output/'meter-graph.json').write_text(json.dumps(graph(),indent=2))
        assert starts('mock-peak-meter','hfp') == 0, 'KDE-style peak meter activated HFP'
        capture = record('peak-real-capture', source)
        wait_for(lambda: current_profile() == 'hfp', 'genuine microphone capture did not activate HFP')
        time.sleep(2)
        stop(capture)
        wait_for(lambda: current_profile() == 'a2dp', 'meter prevented return to A2DP after recording')
        time.sleep(1)
        assert starts('mock-peak-meter','hfp') == 1, 'meter restarted HFP after recording'
        stop(meter)
        assert meter.returncode == 0, report('peak-meter')
        m = re.search(r'meter-samples=(\d+)',report('peak-meter'))
        assert m and int(m[1]) > 5, 'passive meter did not follow genuine recording'
        finish(mock,bridge)
        checks.append({'case':'peak-meter','doesNotActivateHfp':True,'followsRealCapture':True,'returnsA2dpWithMeterOpen':True,'peakSamples':int(m[1])})
    if 'vlc' in a.cases.split(','):
        assert a.vlc, '--vlc is required'
        import wave
        env['PULSE_SERVER'] = 'unix:/tmp/runtime/pulse/native'
        start('pulse', [str(a.pipewire / 'bin/pipewire-pulse')])
        wait_for(lambda: Path('/tmp/runtime/pulse/native').exists(), 'Pulse socket missing')
        mock, bridge = begin('aac', 'vlc')
        sink = next(n['node.name'] for n in nodes() if n['media.class'] == 'Audio/Sink')
        env['PULSE_SINK'] = sink
        with wave.open('/tmp/test.wav', 'wb') as wav:
            wav.setparams((2,2,48000,0,'NONE','not compressed'))
            wav.writeframes(Path('/tmp/music.raw').read_bytes()[:48000*4*3])
        time.sleep(6)
        player = start('vlc', [str(a.vlc), '--intf=dummy','--no-video','--no-one-instance','--play-and-exit','/tmp/test.wav'])
        try:
            player.wait(timeout=12)
            assert player.returncode == 0
        except subprocess.TimeoutExpired:
            (a.output/'stalled-graph.json').write_text(json.dumps(graph(),indent=2))
            raise AssertionError('VLC failed to start/finish three seconds of audio within 12 seconds')
        finish(mock, bridge)
        m = re.search(r'a2dp_nonzero=(\d+)', report('mock-vlc'))
        assert m and int(m[1]) > 10000, 'VLC produced no meaningful PCM'
        checks.append({'case':'vlc','completed':True,'nonzeroPcmBytes':int(m[1])})
    if 'jitter' in a.cases.split(','):
        for kind, profile in [('speaker', 'a2dp'), ('hfp-only', 'hfp')]:
            mock, bridge = begin(kind, 'jitter-' + kind)
            ids = node_ids()
            sink = next(n['node.name'] for n in nodes() if n['media.class'] == 'Audio/Sink')
            play = playback('jitter-player', sink)
            time.sleep(1)
            for cycle in range(3):
                control('stall')
                time.sleep(.6)
                assert bridge.poll() is None, 'bridge died during temporary transport stall'
                assert node_ids() == ids, 'temporary transport stall replaced endpoint'
                assert starts('mock-jitter-' + kind, profile) == 1, 'transport was restarted'
                control('online')
                time.sleep(.4)
            stop(play)
            finish(mock, bridge)
            checks.append({'case': 'jitter', 'kind': kind, 'cycles': 3, 'stableNodeIds': True})
    if 'pulse-pause' in a.cases.split(','):
        assert a.pulse_tools, '--pulse-tools is required'
        env['PULSE_SERVER'] = 'unix:/tmp/runtime/pulse/native'
        start('pulse', [str(a.pipewire / 'bin/pipewire-pulse')])
        wait_for(lambda: Path('/tmp/runtime/pulse/native').exists(), 'Pulse socket missing')
        mock, bridge = begin('aac', 'pulse-pause')
        sink = next(n['node.name'] for n in nodes() if n['media.class'] == 'Audio/Sink')
        ids = node_ids()
        play = start('pulse-player', [str(a.pulse_tools / 'pacat'), '--raw', '--playback',
            '--rate=48000', '--channels=2', '--format=s16le', '--latency-msec=90',
            '--device=' + sink, '/tmp/music.raw'])
        for cycle in range(8):
            time.sleep(.5)
            subprocess.run([str(a.pulse_tools / 'pactl'), 'suspend-sink', sink, '1'], env=env, check=True, timeout=5)
            time.sleep(.2)
            assert bridge.poll() is None, ('bridge crashed on suspension', cycle)
            subprocess.run([str(a.pulse_tools / 'pactl'), 'suspend-sink', sink, '0'], env=env, check=True, timeout=5)
            inputs = subprocess.check_output([str(a.pulse_tools / 'pactl'), 'list', 'short', 'sink-inputs'], env=env, text=True)
            stream_id = inputs.split()[0]
            subprocess.run([str(a.pulse_tools / 'pactl'), 'move-sink-input', stream_id, 'test_builtin_feeder'], env=env, check=True, timeout=5)
            time.sleep(.2)
            assert bridge.poll() is None, ('bridge crashed on unlink', cycle)
            subprocess.run([str(a.pulse_tools / 'pactl'), 'move-sink-input', stream_id, sink], env=env, check=True, timeout=5)
            assert node_ids() == ids, 'pause replaced Bluetooth nodes'
        stop(play)
        time.sleep(2)
        assert bridge.poll() is None, 'bridge crashed on player disconnect'
        finish(mock, bridge)
        checks.append({'case': 'pulse-pause', 'cycles': 8, 'stableNodeIds': True})
    for kind in (item for item in a.cases.split(',') if item not in ('peak-meter', 'vlc', 'lifecycle', 'denied', 'hfp-only', 'defaults', 'pulse-pause', 'jitter')):
        mock, bridge = begin(kind, kind)
        mock_name = 'mock-' + kind
        props = nodes()
        for node in props:
            assert node.get('node.virtual') is False, 'physical endpoint marked virtual'
            assert node.get('device.bus') == 'bluetooth', 'Bluetooth identity missing'
            assert node.get('node.description') == 'Scripted headset', 'device name missing'
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
            source_id = ids[source]
            source_ports = node_ports(source_id)
            assert len(source_ports) == 1, ('headset microphone must expose one channel', source_ports)
            assert source_ports[0].get('audio.channel') == 'MONO', \
                ('headset microphone channel is not MONO', source_ports)
            # WirePlumber intentionally remembers node volume by stable name.
            # Normalize it before evaluating PCM so the preceding matrix case's
            # persistence check cannot lower the next case's fixture tone.
            subprocess.run([str(a.wireplumber / 'bin/wpctl'), 'set-volume', str(source_id), '100%'],
                env=env, check=True, timeout=5)
            assert abs(volume(source_id) - 1.0) < .011, ('source volume was not normalized', volume(source_id))
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
            subprocess.run([str(a.wireplumber / 'bin/wpctl'), 'set-volume', str(source_id), '37%'],
                env=env, check=True, timeout=5)
            assert abs(volume(source_id) - .37) < .011, ('source volume was not applied', volume(source_id))
            # Brief consumer restart must not release and reacquire transport.
            time.sleep(.25)
            capture2 = record(kind + '-headset-record2', source)
            time.sleep(.75)
            assert starts(mock_name, 'hfp') == 1, 'rapid recording restart caused churn'
            stop(capture2)
            wait_for(lambda: current_profile() == 'a2dp', 'idle microphone did not return A2DP')
            assert node_ids() == ids, 'node identities changed leaving HFP'
            assert abs(volume(source_id) - .37) < .011, ('source volume lost across profile round trip', volume(source_id))
            data = Path('/tmp/' + kind + '-headset-record.raw').read_bytes()
            assert len(data) >= 16000 and any(data), ('microphone PCM absent', kind, len(data))
            metrics = pcm_metrics(data, 16000)
            assert 475 < metrics['toneHz'] < 525, ('microphone rate conversion corrupted tone', kind, metrics)
            case['captureMetrics'] = metrics
            case.update(headsetMicSwitches=True, passiveMeterDoesNotSwitch=True,
                concurrentDuplex=True, recordingRestartDebounced=True, returnsA2dp=True,
                stableNodeIds=True, capturedBytes=len(data))
            case.update(monoSourceChannel=True, sourceVolumePersists=True)
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
        source_id = node_ids()[source]
        subprocess.run([str(a.wireplumber / 'bin/wpctl'), 'set-volume', str(source_id), '100%'],
            env=env, check=True, timeout=5)
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
        recovered = record('reconnected-call', source)
        wait_for(lambda: current_profile() == 'hfp', 'reconnected microphone did not start HFP')
        time.sleep(1.5); stop(recovered)
        wait_for(lambda: current_profile() == 'a2dp', 'reconnected microphone did not return A2DP')
        recovered_data = Path('/tmp/reconnected-call.raw').read_bytes()
        assert len(recovered_data) >= 16000 and any(recovered_data), \
            ('reconnected microphone PCM absent', len(recovered_data))
        recovered_metrics = pcm_metrics(recovered_data, 16000)
        assert 475 < recovered_metrics['toneHz'] < 525, ('reconnected microphone tone corrupted', recovered_metrics)
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
        checks[-1].update(reconnectedCaptureBytes=len(recovered_data), reconnectedCaptureMetrics=recovered_metrics)
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
    'bridgeExecutable': os.environ.get('SMOKE_BRIDGE_ORIGIN', str(a.bridge)),
    'bridgeSha256': hashlib.sha256(a.bridge.read_bytes()).hexdigest(),
    'checks': checks,
    'limitations': 'Scripted capability and post-Floss PCM peers only. CVSD/mSBC cases validate their 8/16 kHz bridge contracts, not SCO codec negotiation or bitstreams. Advertised AAC is not proof of actual AAC negotiation or encoding. No controller, radio, or headset.'}, indent=2) + '\n')
print(json.dumps(checks, indent=2))
