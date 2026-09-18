#!/usr/bin/env python3
"""Load built BlueDevil on an offscreen display and activation-free private buses."""
import argparse
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import time


def pairing_worker(data, env):
    """Exercise the shipped agent and a real PairingWindow on a private bus."""
    children = []
    agent = None
    def call(destination, path, interface, method):
        return subprocess.check_output(['busctl', '--user', 'call', destination, path,
                                        interface, method], env=env, text=True).strip()
    def status():
        return [int(value) for value in call('org.chromium.bluetooth',
            '/org/chromium/bluetooth/Test', 'org.chromium.bluetooth.Test', 'GetStatus').split()[1:]]
    def wait_for(predicate, seconds=5):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if predicate():
                return
            time.sleep(0.05)
        raise RuntimeError('pairing worker condition timed out')
    def owner_pid():
        result = subprocess.run(['busctl', '--user', 'call', 'org.freedesktop.DBus',
            '/org/freedesktop/DBus', 'org.freedesktop.DBus', 'GetConnectionUnixProcessID',
            's', 'org.kde.bluedevil.FlossPairing'], env=env, text=True, capture_output=True)
        return int(result.stdout.split()[1]) if result.returncode == 0 else None
    try:
        ready = Path(env['XDG_RUNTIME_DIR']) / 'mock-ready'
        ready.unlink(missing_ok=True)
        env['FLOSS_MOCK_READY'] = str(ready)
        mock = subprocess.Popen([data['mock']], env=env)
        children.append(mock)
        wait_for(ready.exists)
        handoff = data['scenario'] == 'pairing-handoff'
        if handoff:
            agent = subprocess.Popen([data['agent']], env=env)
            children.append(agent)
            wait_for(lambda: status()[0] == 1)
            wait_for(lambda: owner_pid() == agent.pid)
        ui_log = Path(env['XDG_RUNTIME_DIR']) / 'pairing-ui.log'
        with ui_log.open('w') as stream:
            ui = subprocess.Popen(data['command'], env=env, stdout=stream, stderr=subprocess.STDOUT)
            children.append(ui)
            wait_for(lambda: 'MOCK_PAIRING_UI_READY_IDLE_HIDDEN' in ui_log.read_text())
            if not handoff:
                wait_for(lambda: owner_pid() == ui.pid)
                agent = subprocess.Popen([data['agent']], env=env)
                children.append(agent)
            wait_for(lambda: status()[0] == 2)
            time.sleep(0.3)
            assert owner_pid() == (agent.pid if handoff else ui.pid), 'pairing owner was stolen'
            call('org.chromium.bluetooth', '/org/chromium/bluetooth/Test',
                 'org.chromium.bluetooth.Test', 'TriggerPairing')
            if handoff:
                time.sleep(0.8)
                assert status()[1:] == [0, 0], 'agent automatically answered or cancelled'
                assert 'MOCK_PAIRING_HANDOFF_CANCELLED' not in ui_log.read_text(), 'observer answered before ownership'
                assert agent.poll() is None, 'agent exited on its visible prompt'
                agent.terminate()
                agent.wait(timeout=3)
                print('MOCK_PAIRING_AGENT_TERMINATED', agent.returncode, flush=True)
            ui.wait(timeout=8)
            assert ui.returncode == 0, 'pairing UI failed'
        print(ui_log.read_text(), flush=True)
        time.sleep(0.2)
        assert status()[1:] == ([0, 1] if handoff else [1, 0]), 'wrong/duplicate response counts'
        if agent.poll() is None:
            agent.terminate()
            agent.wait(timeout=3)
            print('MOCK_PAIRING_AGENT_TERMINATED', agent.returncode, flush=True)
        assert agent.returncode in (0, -signal.SIGTERM), 'agent crashed'
        print('MOCK_PAIRING_WORKER_PASS', flush=True)
        return 0
    except (AssertionError, RuntimeError, OSError, subprocess.SubprocessError) as error:
        print('MOCK_PAIRING_WORKER_FAIL', str(error), flush=True)
        if 'ui_log' in locals() and ui_log.exists():
            print(ui_log.read_text(), flush=True)
        return 1
    finally:
        for child in reversed(children):
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()


def main():
    if len(sys.argv) > 1 and sys.argv[1] == '--worker':
        data = json.loads(Path(sys.argv[2]).read_text())
        env = data['env']
        env['DBUS_SESSION_BUS_ADDRESS'] = os.environ['DBUS_SESSION_BUS_ADDRESS']
        env['DBUS_SYSTEM_BUS_ADDRESS'] = env['DBUS_SESSION_BUS_ADDRESS']
        if data.get('agent'):
            return pairing_worker(data, env)
        if data.get('mock'):
            ready = Path(env['XDG_RUNTIME_DIR']) / 'mock-ready'
            env['FLOSS_MOCK_READY'] = str(ready)
            env['FLOSS_MOCK_SCENARIO'] = data.get('scenario', '')
            ready.unlink(missing_ok=True)
            Path(str(ready) + "-reconnected").unlink(missing_ok=True)
            observer = None
            policy = None
            mock = subprocess.Popen([data['mock']], env=env)
            try:
                for _ in range(100):
                    if ready.exists():
                        if data.get('policy'):
                            policy = subprocess.Popen([data['policy']], env=env)
                            time.sleep(0.2)
                        if data.get('observer'):
                            observer = subprocess.Popen(data['observer'], env=env)
                        result = subprocess.call(data['command'], env=env)
                        if data.get('headless') and result == 0:
                            for _ in range(450):
                                if Path(str(ready) + '-reconnected').exists():
                                    return 0
                                if policy.poll() is not None:
                                    return 1
                                time.sleep(0.1)
                            return 1
                        return result
                    if mock.poll() is not None:
                        return 1
                    time.sleep(0.02)
                return 1
            finally:
                if policy is not None:
                    policy.terminate()
                    policy.wait(timeout=3)
                if observer is not None:
                    observer.terminate()
                    observer.wait(timeout=3)
                mock.terminate()
                mock.wait(timeout=3)
        os.execve(data['command'][0], data['command'], env)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bluedevil', required=True)
    parser.add_argument('--plasma', required=True, help='Matching plasma-workspace store output')
    parser.add_argument('--desktop', required=True, help='Matching plasma-desktop store output (shell representations)')
    parser.add_argument('--breeze', required=True, help='Matching qqc2-breeze-style store output')
    parser.add_argument('--output', required=True)
    parser.add_argument('--mock-helper', help='Optional compiled tools/kde-mock/floss-mock.cpp executable')
    parser.add_argument('--case', action='append', choices=['backend', 'kcm', 'applet', 'mock-device', 'mock-reconnect', 'mock-headless', 'mock-pairing-owner', 'mock-pairing-handoff', 'mock-rfkill-hard', 'mock-rfkill-soft', 'mock-rfkill-fail', 'mock-rfkill-absent', 'mock-rfkill-stuck'], help='Run only selected cases (repeatable)')
    args = parser.parse_args()
    roots = [str(Path(p).resolve()) for p in (args.bluedevil, args.plasma, args.desktop, args.breeze)]
    closure = subprocess.check_output(['nix-store', '-qR', *roots], text=True).splitlines()
    paths = list(dict.fromkeys(roots + closure))
    def executable(name):
        for path in paths:
            candidate = Path(path) / 'bin' / name
            if candidate.is_file():
                return str(candidate)
        raise RuntimeError(f'{name} absent from supplied package closures')
    def directories(suffix):
        return ':'.join(str(Path(p) / suffix) for p in paths if (Path(p) / suffix).is_dir())
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = {}
    (output / 'summary.json').unlink(missing_ok=True)
    with tempfile.TemporaryDirectory(prefix='floss-kde-smoke-') as temporary:
        base = Path(temporary)
        for name in ('runtime', 'home', 'config', 'cache', 'state'):
            (base / name).mkdir(mode=0o700)
        env = {'PATH': os.environ['PATH'], 'HOME': str(base / 'home'), 'LANG': 'C.UTF-8',
               'XDG_RUNTIME_DIR': str(base / 'runtime'), 'XDG_CONFIG_HOME': str(base / 'config'),
               'XDG_CACHE_HOME': str(base / 'cache'), 'XDG_STATE_HOME': str(base / 'state'),
               'XDG_DATA_DIRS': directories('share'), 'QT_PLUGIN_PATH': directories('lib/qt-6/plugins'),
               'QML_IMPORT_PATH': directories('lib/qt-6/qml'), 'QT_QPA_PLATFORM': 'offscreen',
               'QT_QUICK_BACKEND': 'software', 'QT_QUICK_CONTROLS_STYLE': 'Basic',
               'QT_FORCE_STDERR_LOGGING': '1', 'QT_LOGGING_RULES': '*.debug=false;qml.debug=true'}
        config = base / 'dbus.conf'
        config.write_text(f'''<busconfig><type>session</type><listen>unix:tmpdir={base / 'runtime'}</listen>
<auth>EXTERNAL</auth><policy context="default"><allow send_destination="*"/>
<allow receive_sender="*"/><allow own="*"/></policy></busconfig>''')
        qml = base / 'backend.qml'
        qml.write_text('''import QtQuick
import org.kde.bluedevil.floss
Item {
    Component.onCompleted: {
        console.log("FLOSS_BACKEND_LOADED", FlossBackend.available, FlossBackend.devices.length);
        let view = FlossBackend.attachPairingView();
        console.log("PAIRING_VIEW", view.length > 0);
        FlossBackend.detachPairingView(view);
    }
    Timer { interval: 1500; running: true; onTriggered: Qt.quit() }
}
''')
        cases = {'backend': [executable('qml'), str(qml)],
                 'kcm': [executable('kcmshell6'), 'kcm_bluetooth'],
                 'applet': [executable('plasmawindowed'), 'org.kde.plasma.bluetooth']}
        if args.mock_helper:
            cases['mock-device'] = [executable('qml'), str(Path(__file__).parent.resolve() / 'kde-mock/backend-scenario.qml')]
            cases['mock-reconnect'] = [executable('qml'), str(Path(__file__).parent.resolve() / 'kde-mock/reconnect-scenario.qml')]
            cases['mock-headless'] = [executable('qml'), str(qml)]
        if args.mock_helper:
            for variant in ('hard', 'soft', 'fail', 'absent', 'stuck'):
                cases['mock-rfkill-' + variant] = [executable('qml'), str(Path(__file__).parent.resolve() / 'kde-mock/rfkill-scenario.qml'), '--', variant]
            for variant in ('owner', 'handoff'):
                cases['mock-pairing-' + variant] = [executable('qml'), str(Path(__file__).parent.resolve() / 'kde-mock/pairing-scenario.qml'), '--', variant]
        if args.case:
            missing = set(args.case) - cases.keys()
            if missing:
                parser.error('Selected mock cases require --mock-helper')
            cases = {name: command for name, command in cases.items() if name in args.case}
        errors = re.compile(r'error when loading applet|Error loading QML|Component is not ready|Cannot assign to non-existent|Type .* unavailable|module .* is not installed|ReferenceError:|TypeError:|Failed to load')
        for name, command in cases.items():
            case_config = base / f'config-{name}'
            case_config.mkdir()
            env['XDG_CONFIG_HOME'] = str(case_config)
            data = base / f'{name}.json'
            data.write_text(json.dumps({'env': env, 'command': command,
                                       'mock': str(Path(args.mock_helper).resolve()) if name.startswith('mock-') else None,
                                       'scenario': name.removeprefix('mock-') if name.startswith(('mock-pairing-', 'mock-rfkill-')) else 'reconnect' if name in ('mock-reconnect', 'mock-headless') else '',
                                       'agent': str(Path(roots[0]) / 'bin/bluedevil-floss-pairing') if name.startswith('mock-pairing-') else None,
                                       'policy': str(Path(roots[0]) / 'bin/bluedevil-floss-policy') if name in ('mock-reconnect', 'mock-headless') else None,
                                       'headless': name == 'mock-headless',
                                       'observer': [executable('qml'), str(Path(__file__).parent.resolve() / 'kde-mock/observer.qml')] if name == 'mock-reconnect' else None}))
            timed_out = False
            with (output / f'{name}.log').open('w') as log:
                proc = subprocess.Popen(['dbus-run-session', f'--config-file={config}', '--',
                                         sys.executable, str(Path(__file__).resolve()), '--worker', str(data)],
                                        stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
                try:
                    proc.wait(timeout=160 if name in ('mock-reconnect', 'mock-headless') else 30 if name == 'mock-device' or name.startswith(('mock-pairing-', 'mock-rfkill-')) else 8)
                except subprocess.TimeoutExpired:
                    timed_out = True
                finally:
                    # Also clean up any child processes left after the foreground program exits.
                    try:
                        os.killpg(proc.pid, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                    try:
                        proc.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        os.killpg(proc.pid, signal.SIGKILL)
                        proc.wait()
            text = (output / f'{name}.log').read_text()
            failure_lines = [line for line in text.splitlines() if errors.search(line)]
            passed = not failure_lines and (timed_out if name != 'backend' else
                     proc.returncode == 0 and 'FLOSS_BACKEND_LOADED false 0' in text and 'PAIRING_VIEW true' in text)
            if name == 'mock-device':
                required = ['Start i', 'Stop i', 'StartDiscovery', 'CancelDiscovery', 'CreateBond a{sv}u',
                            'SetPairingConfirmation a{sv}b', 'ConnectAllEnabledProfiles a{sv}',
                            'DisconnectAllEnabledProfiles a{sv}', 'RemoveBond a{sv}']
                passed = (proc.returncode == 0 and not failure_lines and 'MOCK_SCENARIO_PASS' in text
                          and 'MOCK_SCENARIO_FAIL' not in text and 'BAD_' not in text
                          and 'SPOOF_REJECTED' in text and 'BATTERY_SPOOF_REJECTED' in text
                          and 'MOCK_ADAPTER_OWNER_REMOVED' in text
                          and all('METHOD ' + method in text for method in required))
            if name == 'mock-reconnect':
                attempts = [int(m) for m in re.findall(r'METHOD ConnectAllEnabledProfiles a\{sv\} AT (\d+)', text)]
                passed = (proc.returncode == 0 and not failure_lines and 'MOCK_SCENARIO_PASS' in text
                          and 'MOCK_SCENARIO_FAIL' not in text and 'BAD_' not in text
                          and 'MOCK_CONNECTION_REJECTED' in text and 'MOCK_LINK_DROPPED' in text
                          and 'MOCK_DAEMON_RESTART_END' in text and 'MOCK_OBSERVER' in text
                          and 'MOCK_LATE_CONNECTION' in text and 'MOCK_LATE_CONNECTION_RECONCILED' in text
                          and len(attempts) == 4 and attempts[1] - attempts[0] >= 4000)
            if name == 'mock-headless':
                passed = (proc.returncode == 0 and not failure_lines and 'BAD_' not in text
                          and text.count('METHOD ConnectAllEnabledProfiles a{sv}') == 4
                          and 'MOCK_HEADLESS_RECOVERY_COMPLETE' in text)
            if name.startswith('mock-pairing-'):
                passed = (proc.returncode == 0 and not failure_lines and 'MOCK_PAIRING_WORKER_PASS' in text
                          and text.count('PAIRING_SPOOF_REJECTED') == 2
                          and 'MOCK_PAIRING_PASS' in text and 'MOCK_PAIRING_FAIL' not in text and 'BAD_' not in text)
            if name.startswith('mock-rfkill-'):
                mode = name.removeprefix('mock-rfkill-')
                passed = (proc.returncode == 0 and not failure_lines and 'MOCK_RFKILL_PASS' in text
                          and 'MOCK_RFKILL_FAIL' not in text and 'BAD_' not in text
                          and text.count('METHOD Start i ') == (0 if mode in ('hard', 'fail', 'stuck') else 1)
                          and text.count('METHOD Stop i ') == 1
                          and text.count('METHOD SetRfkillBlocked ib ') == (1 if mode in ('soft', 'fail', 'stuck') else 0))
                if mode == 'stuck':
                    reads = [int(value) for value in re.findall(r'METHOD GetRfkillState i AT (\d+)', text)]
                    writes = [int(value) for value in re.findall(r'METHOD SetRfkillBlocked ib AT (\d+)', text)]
                    passed = passed and len(reads) >= 11 and len(writes) == 1 and reads[-1] - writes[0] >= 1700
            report[name] = {'passed': passed, 'stopped_after_observation': timed_out,
                            'returncode': proc.returncode, 'qml_errors': failure_lines}
        report['scope'] = ('Offscreen initialization; optional device scenario uses a mock daemon. '
                           'No real daemon, physical pairing, hardware or host Plasma session tested.')
        report['packages'] = roots
        (output / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return 0 if all(report[name]['passed'] for name in cases) else 1


if __name__ == '__main__':
    sys.exit(main())
