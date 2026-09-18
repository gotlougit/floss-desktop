#!/usr/bin/env python3
"""Run the real Floss adapter against a scripted, private HCI socket peer."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import resource
import subprocess
import sys
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--floss', required=True, type=Path)
p.add_argument('--pipewire', type=Path, help='Also check actual bridge rejection for a disconnected device')
p.add_argument('--socket-client', type=Path, help='Cached private-bus SocketManager authorization probe')
p.add_argument('--shim', required=True, type=Path)
p.add_argument('--output', required=True, type=Path)
p.add_argument('--self-check', type=Path, help='Run a fixture client instead of the actual daemon')
p.add_argument('--inside', action='store_true', help=argparse.SUPPRESS)
a = p.parse_args()
if not a.inside:
    a.output.mkdir(parents=True, exist_ok=True)
    (a.output / 'summary.json').unlink(missing_ok=True)
    script = Path(__file__).resolve()
    command = [shutil.which('bwrap'), '--die-with-parent', '--unshare-all', '--cap-drop', 'ALL',
        '--tmpfs', '/', '--ro-bind', '/nix/store', '/nix/store', '--ro-bind', '/etc', '/etc',
        '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp', '--tmpfs', '/run', '--tmpfs', '/var',
        '--dir', '/sys', '--ro-bind', str(script), '/smoke.py',
        '--ro-bind', str(script.parent / 'mock-hci-controller.py'), '/controller.py',
        '--ro-bind', str(a.shim.resolve()), '/shim.so', '--bind', str(a.output.resolve()), '/report',
        '--setenv', 'SMOKE_DBUS', str(Path(shutil.which('dbus-daemon')).resolve()),
        '--setenv', 'SMOKE_BUSCTL', str(Path(shutil.which('busctl')).resolve()),
        str(Path(sys.executable).resolve()), '/smoke.py', '--inside', '--floss', str(a.floss.resolve()),
        '--shim', '/shim.so', '--output', '/report']
    if a.pipewire:
        command += ['--pipewire', str(a.pipewire.resolve())]
    if a.socket_client:
        position = command.index('--setenv')
        command[position:position] = ['--ro-bind', str(a.socket_client.resolve()), '/socket-client']
        command += ['--socket-client', '/socket-client']
    if a.self_check:
        position = command.index('--setenv')
        command[position:position] = ['--ro-bind', str(a.self_check.resolve()), '/self-check']
        command += ['--self-check', '/self-check']
    sys.exit(subprocess.call(command))
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
Path('/var/run').symlink_to('/run')
for directory in ('/tmp/runtime', '/tmp/home', '/run/bluetooth/audio', '/var/lib/bluetooth', '/var/log/bluetooth'):
    Path(directory).mkdir(mode=0o700, parents=True, exist_ok=True)
env = os.environ.copy()
env.update(HOME='/tmp/home', XDG_RUNTIME_DIR='/tmp/runtime',
    DBUS_SYSTEM_BUS_ADDRESS='unix:path=/tmp/private-bus', DBUS_SESSION_BUS_ADDRESS='unix:path=/tmp/private-bus')
Path('/tmp/bus.conf').write_text('''<busconfig><type>session</type><listen>unix:path=/tmp/private-bus</listen>
<policy context="default"><allow own="*"/><allow send_destination="*"/><allow receive_sender="*"/></policy></busconfig>''')
children, logs = [], []
def start(name, command, child_env=env):
    log = (a.output / (name + '.log')).open('w'); logs.append(log)
    child = subprocess.Popen(command, env=child_env, stdout=log, stderr=subprocess.STDOUT)
    children.append(child); return child

def wait_for(predicate, message, seconds=15):
    deadline = time.monotonic() + seconds
    while not predicate():
        assert time.monotonic() < deadline, message
        time.sleep(.1)

def call(path, interface, method, *args):
    return subprocess.run([os.environ['SMOKE_BUSCTL'], '--address=' + env['DBUS_SYSTEM_BUS_ADDRESS'],
        '--timeout=2', 'call', 'org.chromium.bluetooth', '/org/chromium/bluetooth/hci0/' + path,
        'org.chromium.bluetooth.' + interface, method, *args], env=env, capture_output=True, text=True, timeout=4)

results = {}
try:
    start('bus', [os.environ['SMOKE_DBUS'], '--nofork', '--config-file=/tmp/bus.conf'])
    wait_for(lambda: Path('/tmp/private-bus').exists(), 'private bus missing')
    controller = start('controller', [sys.executable, '/controller.py', '/tmp/hci', '--syslog', '/dev/log'])
    wait_for(lambda: Path('/tmp/hci/controller').exists(), 'scripted controller missing')
    adapter_env = dict(env, LD_PRELOAD=str(a.shim), MOCK_HCI_DIRECTORY='/tmp/hci')
    if a.self_check:
        client = start('fixture-self-check', [str(a.self_check)], adapter_env)
        assert client.wait(timeout=5) == 0, 'fixture self-check failed'
        results['fixtureSelfCheckOnly'] = True
    else:
        adapter = start('adapter', [str(a.floss / 'bin/btadapterd'), '--hci=0', '--index=0', '--log-output=stderr'], adapter_env)
        def ready():
            assert adapter.poll() is None, ('adapter exited', adapter.returncode)
            assert controller.poll() is None, 'controller fixture exited'
            response = call('adapter', 'Bluetooth', 'GetAddress')
            results['address'] = response.stdout.strip()
            if response.returncode != 0 or '11:22:33:44:55:66' not in response.stdout:
                return False
            media = call('media', 'BluetoothMedia', 'IsInitialized')
            return media.returncode == 0 and media.stdout.strip() == 'b true'
        wait_for(ready, 'controller address never became available', 30)
        for path, interface, method, arguments in (
            ('adapter', 'Bluetooth', 'GetName', []),
            ('adapter', 'Bluetooth', 'GetUuids', []),
            ('media', 'BluetoothMedia', 'IsInitialized', []),
            ('media', 'BluetoothMedia', 'GetHfpPcmConfig', ['s', 'AA:BB:CC:DD:EE:FF']),
            ('media', 'BluetoothMedia', 'GetConnectedAudioDevices', []),
            ('media', 'BluetoothMedia', 'GetA2dpPcmConfig', ['to', '42', '/org/pipewire/FlossAudio/Test']),
        ):
            response = call(path, interface, method, *arguments)
            (a.output / (method + '.log')).write_text(response.stdout + response.stderr)
            assert response.returncode == 0, (method, response.stderr)
            results[method] = response.stdout.strip()
        assert results['IsInitialized'] == 'b true', results
        assert '"ready" b false' in results['GetHfpPcmConfig'], results
        assert results['GetConnectedAudioDevices'] == 'aa{sv} 0', results
        assert '"ready" b false' in results['GetA2dpPcmConfig'], results
        changed = call('adapter', 'Bluetooth', 'SetName', 's', 'Floss smoke test')
        assert changed.returncode == 0 and changed.stdout.strip() == 'b true', changed.stderr
        def renamed():
            response = call('adapter', 'Bluetooth', 'GetName')
            return response.returncode == 0 and response.stdout.strip() == 's "Floss smoke test"'
        wait_for(renamed, 'name update did not reach native stack')
        results['adapterNameRoundTrip'] = True
        time.sleep(2)
        assert adapter.poll() is None and controller.poll() is None
        results['adapterAliveAfterInitialization'] = True
        if a.socket_client:
            probe = subprocess.run([str(a.socket_client)], env=env, capture_output=True, text=True, timeout=20)
            (a.output / 'socket-authorization.log').write_text(probe.stdout + probe.stderr)
            assert probe.returncode == 0, ('SocketManager authorization probe failed', probe.stdout, probe.stderr)
            authorization = json.loads(probe.stdout)
            assert authorization.get('passed') is True, authorization
            authorization['clientSha256'] = hashlib.sha256(a.socket_client.read_bytes()).hexdigest()
            results['socketManagerAuthorization'] = authorization
            assert adapter.poll() is None, 'adapter exited during socket authorization checks'
        if a.pipewire:
            for profile in ('hfp', 'a2dp'):
                command = [str(a.pipewire / 'bin/pw-floss'), '--profile', profile,
                    '--device', 'AA:BB:CC:DD:EE:FF']
                bridge = subprocess.run(command, env=env, capture_output=True, text=True, timeout=8)
                (a.output / ('actual-bridge-' + profile + '.log')).write_text(bridge.stdout + bridge.stderr)
                assert bridge.returncode == 1 and 'refused the address-scoped audio reservation' in bridge.stderr, bridge.stderr
                results['disconnectedDeviceRejected-' + profile] = True
            assert adapter.poll() is None
        # main.rs bounds shutdown waits to 4s + 11s + 1s; allow scheduling margin.
        shutdown_started = time.monotonic()
        adapter.terminate()
        assert adapter.wait(timeout=20) == 0, ('adapter shutdown failed', adapter.returncode)
        results['adapterShutdownExitCode'] = adapter.returncode
        results['adapterShutdownSeconds'] = round(time.monotonic() - shutdown_started, 3)
finally:
    for child in reversed(children):
        if child.poll() is None:
            child.terminate()
            try: child.wait(timeout=5)
            except subprocess.TimeoutExpired: child.kill(); child.wait()
    for log in logs: log.close()
(a.output / 'summary.json').write_text(json.dumps({'flossPackage': str(a.floss), 'checks': results,
    'limitations': 'Scripted controller startup via LD_PRELOAD socket interception; no Linux HCI ownership, USB, radio, pairing, remote links or headset.'}, indent=2) + '\n')
print(json.dumps(results, indent=2))
