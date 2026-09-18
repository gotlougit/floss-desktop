import QtQuick
import org.kde.bluedevil.floss
Item {
    property int stage: 0
    property int ticks: 0
    property string mode: Qt.application.arguments[Qt.application.arguments.length - 1]
    function fail(message) { console.error("MOCK_RFKILL_FAIL", message); Qt.exit(1); }
    Timer {
        interval: 50; running: true; repeat: true
        onTriggered: {
            ticks++;
            if (ticks > 300) { fail("timeout at stage " + stage); return; }
            if (stage === 0 && FlossBackend.available && ticks > 10) {
                FlossBackend.setPowered(false);
                stage = 1;
            } else if (stage === 1 && !FlossBackend.available && !FlossBackend.powerPending) {
                FlossBackend.setPowered(true);
                stage = 2;
            } else if (stage === 2 && !FlossBackend.powerPending) {
                if (mode === "hard" || mode === "fail" || mode === "stuck") {
                    if (FlossBackend.available || FlossBackend.error.length === 0) { fail("blocked enable succeeded or no error"); return; }
                    if (mode === "hard" && !FlossBackend.hardBlocked) { fail("hard block missing"); return; }
                } else {
                    if (!FlossBackend.available) return;
                    if (FlossBackend.softBlocked || FlossBackend.hardBlocked) { fail("stale block state"); return; }
                }
                stage = 3;
                ticks = 0;
            } else if (stage === 3 && ticks >= 6) {
                // Let authentication probes queued on adapter re-registration
                // complete before destroying their callback target.
                console.log("MOCK_RFKILL_PASS", mode);
                Qt.exit(0);
            }
        }
    }
}
