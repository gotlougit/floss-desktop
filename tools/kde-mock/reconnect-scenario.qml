import QtQuick
import org.kde.bluedevil.floss
Item {
    property int stage: 0
    property int ticks: 0
    property int inhibitTick: 0
    readonly property string address: "12:34:56:78:9A:BC"
    function fail(message) { console.error("MOCK_SCENARIO_FAIL", message); Qt.exit(1); }
    Timer {
        interval: 100; running: true; repeat: true
        onTriggered: {
            ticks++;
            if (ticks > 1500) { fail("reconnect timeout stage " + stage); return; }
            if (FlossBackend.available && FlossBackend.error.length > 0) { fail("automatic retry exposed UI error: " + FlossBackend.error); return; }
            const d = FlossBackend.devices.find(d => d.address === address);
            if (stage === 0 && d && d.paired && d.connected) {
                console.log("MOCK_CHECK initial paired device auto connected after rejection"); stage = 1;
            } else if (stage === 1 && d && !d.connected) { stage = 2;
            } else if (stage === 2 && d && d.connected) {
                console.log("MOCK_CHECK link loss reconnected"); stage = 3;
            } else if (stage === 3 && !FlossBackend.available) { stage = 4;
            } else if (stage === 4 && d && d.connected) {
                console.log("MOCK_CHECK daemon restart reconnected");
                FlossBackend.operate(address, "disconnect"); stage = 5;
            } else if (stage === 5 && d && !d.connected && !d.pending) { inhibitTick = ticks; stage = 6;
            } else if (stage === 6) {
                if (ticks - inhibitTick > 100) {
                    if (d && d.connected) { fail("late connection was not disconnected"); return; }
                    console.log("MOCK_CHECK explicit disconnect inhibited ten seconds");
                    FlossBackend.operate(address, "forget"); stage = 7;
                }
            } else if (stage === 7 && d && !d.paired && !d.pending) {
                inhibitTick = ticks; stage = 8;
            } else if (stage === 8) {
                if (d && (d.connected || d.paired)) { fail("forgotten device reconnected"); return; }
                if (ticks - inhibitTick > 60) {
                    console.log("MOCK_CHECK forgotten device remains disconnected");
                    console.log("MOCK_SCENARIO_PASS"); Qt.quit();
                }
            }
        }
    }
}
