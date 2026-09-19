#!/usr/bin/env python3
"""Read KDE's installed audio-output model, without modifying audio settings.

Unlike the isolated mock suites, this intentionally inspects the current
user's PulseAudio server. Requires an already installed plasma-pa package.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--plasma-pa', required=True, type=Path)
args = parser.parse_args()
root = str(args.plasma_pa.resolve())
paths = list(dict.fromkeys([root] + subprocess.check_output(
    ['nix-store', '-qR', root], text=True).splitlines()))
env = os.environ.copy()
for key, suffix in [('QML_IMPORT_PATH', 'lib/qt-6/qml'),
                    ('QT_PLUGIN_PATH', 'lib/qt-6/plugins')]:
    env[key] = ':'.join(str(Path(p) / suffix) for p in paths if (Path(p) / suffix).is_dir())
env.update(QT_QPA_PLATFORM='offscreen', QT_QUICK_BACKEND='software',
           QT_FORCE_STDERR_LOGGING='1', QT_LOGGING_RULES='qml.debug=true')
qml = next(str(Path(p) / 'bin/qml') for p in paths if (Path(p) / 'bin/qml').is_file())
with tempfile.TemporaryDirectory(prefix='floss-audio-panel-') as directory:
    probe = Path(directory) / 'probe.qml'
    probe.write_text('import QtQuick\nimport org.kde.kitemmodels as KItemModels\nimport org.kde.plasma.private.volume\nItem {\n    SinkModel { id: sinks }\n    PulseObjectFilterModel { id: visible; sourceModel: sinks; filterVirtualDevices: true }\n    Timer { interval: 2000; running: true; onTriggered: {\n        let found = false;\n        for (let row = 0; row < visible.rowCount(); ++row) {\n            const idx = visible.index(row, 0);\n            const name = visible.data(idx, visible.KItemModels.KRoleNames.role("Name"));\n            const obj = visible.data(idx, visible.KItemModels.KRoleNames.role("PulseObject"));\n            console.log("OUTPUT", name, obj.description, "virtual=" + obj.virtualDevice);\n            if (name.startsWith("floss_output.")) found = true;\n        }\n        console.log("FLOSS_VISIBLE", found);\n        Qt.exit(found ? 0 : 2);\n    } }\n}\n')
    result = subprocess.run([qml, str(probe)], env=env, timeout=15)
raise SystemExit(result.returncode)
