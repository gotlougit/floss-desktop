import QtQuick
import org.kde.bluedevil.floss
Item {
    property int stage: 0
    property int ticks: 0
    property string view: ""
    readonly property string address: "12:34:56:78:9A:BC"
    Component.onCompleted: view = FlossBackend.attachPairingView()
    function fail(message) { console.error("MOCK_SCENARIO_FAIL", message); Qt.exit(1); }
    Timer {
        interval: 50; running: true; repeat: true
        onTriggered: {
            ticks++;
            if (ticks > 500) { fail("timeout at stage " + stage); return; }
            const devices = FlossBackend.devices;
            const d = devices.length > 0 ? devices[0] : null;
            if (stage > 0 && stage < 4 && FlossBackend.error.length > 0) { fail(FlossBackend.error); return; }
            if (devices.some(device => device.address === "00:00:00:00:00:01")) { fail("spoofed callback accepted"); return; }
            if (stage === 0 && FlossBackend.available) {
                console.log("MOCK_CHECK adapter available");
                FlossBackend.setPowered(true);
                FlossBackend.discover(true);
                stage = 1;
            } else if (stage === 1 && FlossBackend.discovering && d && d.paired === false && d.connected === false) {
                if (d.address !== address || d.name !== "Mock Floss headset") { fail("device properties"); return; }
                console.log("MOCK_CHECK discovered device properties");
                FlossBackend.operate(address, "pair");
                stage = 2;
            } else if (stage === 2 && FlossBackend.pairing.token) {
                const p = FlossBackend.pairing;
                if (p.kind !== "confirm" || p.code !== "123456" || p.address !== address || p.view !== view) { fail("pairing prompt properties"); return; }
                console.log("MOCK_CHECK SSP confirmation prompt");
                FlossBackend.answerPairing(p.token, true);
                stage = 3;
            } else if (stage === 3 && d && d.paired && d.connected && !d.pending && d.batteries && d.batteries.length === 1) {
                if (FlossBackend.pairing.token) { fail("stale pairing prompt"); return; }
                console.log("MOCK_CHECK paired and connected callbacks");
                if (!d.batteries || d.batteries.length !== 1 || d.batteries[0].percentage !== 73) { fail("initial battery snapshot"); return; }
                FlossBackend.setAlias(address, "My headset"); stage = 10;
            } else if (stage === 10 && d && !d.aliasPending && d.alias === "My headset" && d.batteries[0].percentage === 61) {
                if (d.name !== "My headset") { fail("alias display"); return; }
                console.log("MOCK_CHECK alias and battery update");
                FlossBackend.setAlias(address, ""); stage = 11;
            } else if (stage === 11 && d && !d.aliasPending && d.alias === "") {
                if (d.name !== "Mock Floss headset") { fail("alias clearing"); return; }
                FlossBackend.setAlias(address, "Zero battery"); stage = 12;
            } else if (stage === 12 && d && !d.aliasPending && d.alias === "Zero battery" && d.batteries.length === 1 && d.batteries[0].percentage === 0) {
                console.log("MOCK_CHECK zero battery retained");
                FlossBackend.setAlias(address, "Multiple batteries"); stage = 13;
            } else if (stage === 13 && d && !d.aliasPending && d.batteries.length === 2) {
                if (d.batteries[0].percentage !== 54 || d.batteries[1].percentage !== 32) { fail("multi battery values"); return; }
                console.log("MOCK_CHECK multiple battery components");
                FlossBackend.setAlias(address, "Invalid battery"); stage = 14;
            } else if (stage === 14 && d && !d.aliasPending && d.alias === "Invalid battery" && d.batteries.length === 0) {
                console.log("MOCK_CHECK invalid battery omitted");
                FlossBackend.setAlias(address, "My headset"); stage = 18;
            } else if (stage === 18 && d && !d.aliasPending && d.batteries.length === 1 && d.batteries[0].percentage === 61) {
                FlossBackend.setAlias(address, "Absent battery"); stage = 15;
            } else if (stage === 15 && d && !d.aliasPending && d.alias === "Absent battery" && d.batteries.length === 0) {
                FlossBackend.setAlias(address, "Rejected alias"); stage = 16;
            } else if (stage === 16 && d && !d.aliasPending && FlossBackend.error.length > 0) {
                if (d.alias !== "Absent battery") { fail("rejected alias applied"); return; }
                console.log("MOCK_CHECK rejected alias preserved old value");
                FlossBackend.setAlias(address, "Ignored alias"); stage = 17;
            } else if (stage === 17 && d && !d.aliasPending) {
                if (d.alias !== "Absent battery" || FlossBackend.error.length === 0) { fail("unconfirmed alias applied"); return; }
                console.log("MOCK_CHECK void alias setter requires confirmed state");
                FlossBackend.operate(address, "disconnect"); stage = 4;
            } else if (stage === 4 && d && !d.connected && !d.pending) {
                console.log("MOCK_CHECK disconnected callback");
                FlossBackend.operate(address, "forget");
                stage = 5;
            } else if (stage === 5 && d && !d.paired && !d.pending) {
                console.log("MOCK_CHECK forgotten bond callback");
                FlossBackend.discover(false);
                FlossBackend.setPowered(false);
                stage = 6;
            } else if (stage === 6 && !FlossBackend.discovering && !FlossBackend.available && devices.length === 0) {
                console.log("MOCK_CHECK daemon owner loss clears model");
                FlossBackend.detachPairingView(view);
                console.log("MOCK_SCENARIO_PASS");
                Qt.quit();
            }
        }
    }
}
