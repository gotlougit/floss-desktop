import QtQuick
import org.kde.bluedevil.floss
Item {
    Component.onCompleted: console.log("MOCK_OBSERVER", FlossBackend.available)
    Timer {
        interval: 100; running: true; repeat: true
        onTriggered: {
            if (FlossBackend.available && FlossBackend.error.length > 0) {
                console.error("BAD_OBSERVER_AUTO_ERROR", FlossBackend.error); Qt.exit(1);
            }
        }
    }
}
