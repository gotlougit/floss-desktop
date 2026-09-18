import QtQuick
import org.kde.bluedevil.floss
Item {
    // qml's generic runner lacks Plasma's localized context; provide only the
    // translation shim for this state/ownership test, without altering widgets.
    function i18n(message, ...values) {
        for (let index = 0; index < values.length; ++index)
            message = message.replace("%" + (index + 1), values[index]);
        return message;
    }
    property int stage: 0
    property int ticks: 0
    property bool handoff: Qt.application.arguments.indexOf("handoff") >= 0
    PairingWindow { id: prompt }
    function fail(message) { console.error("MOCK_PAIRING_FAIL", message); Qt.exit(1); }
    Timer {
        interval: 50; running: true; repeat: true
        onTriggered: {
            ticks++;
            if (ticks > 400) { fail("timeout at stage " + stage); return; }
            if (stage === 0 && FlossBackend.available && ticks > 10) {
                if (prompt.visible || FlossBackend.pairing.token) { fail("idle window visible"); return; }
                console.log("MOCK_PAIRING_UI_READY_IDLE_HIDDEN");
                stage = 1;
            } else if (stage === 1 && FlossBackend.pairing.token) {
                const request = FlossBackend.pairing;
                if (!prompt.visible || request.kind !== "confirm" || request.code !== "123456"
                    || request.address !== "12:34:56:78:9A:BC") { fail("prompt contents or visibility"); return; }
                if (handoff) {
                    if (!request.uncertain || prompt.canAnswer) { fail("unsafe handoff acceptance"); return; }
                    FlossBackend.answerPairing(request.token, true);
                    FlossBackend.answerPairing(request.token, false);
                    console.log("MOCK_PAIRING_HANDOFF_CANCELLED");
                } else {
                    if (!prompt.canAnswer || request.uncertain) { fail("owner cannot answer"); return; }
                    FlossBackend.answerPairing(request.token, true);
                    FlossBackend.answerPairing(request.token, true);
                    console.log("MOCK_PAIRING_OWNER_ANSWERED");
                }
                stage = 2;
            } else if (stage === 2 && !FlossBackend.pairing.token) {
                if (prompt.visible) { fail("finished window visible"); return; }
                console.log("MOCK_PAIRING_PASS");
                Qt.exit(0);
            }
        }
    }
}
