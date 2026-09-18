// Test double only: no Bluetooth hardware or physical pairing is involved.
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusVirtualObject>
#include <QDebug>
#include <QFile>
#include <QElapsedTimer>
#include <QTimer>
#include <QVariantMap>
using DeviceList = QList<QVariantMap>;

class Mock : public QDBusVirtualObject {
    QDBusConnection bus = QDBusConnection::systemBus();
    QString destination, callbackPath, connectionPath, batteryPath;
    QString alias;
    QVariantMap currentBattery;
    QMap<QString, QString> adapterCallbacks, connectionCallbacks, batteryCallbacks;
    QElapsedTimer elapsed;
    const bool reconnectScenario = qEnvironmentVariable("FLOSS_MOCK_SCENARIO") == "reconnect";
    int connectAttempts = 0, connections = 0;
    uint pairingAnswers = 0, pairingCancellations = 0;
    uint starts = 0, stops = 0, unblocks = 0;
    QString rfkillMode = "absent";
    bool softBlocked = false;
    bool explicitDisconnect = false;
    bool lateConnectionSent = false;
    QVariantMap batterySet(uint percentage) const {
        return {{"address", address}, {"source_uuid", "0000180f-0000-1000-8000-00805f9b34fb"},
                {"source_info", "Mock headset"}, {"batteries", QVariant::fromValue(DeviceList{
                    QVariantMap{{"percentage", percentage}, {"variant", ""}}})}};
    }
    const QString address = "12:34:56:78:9A:BC";
    QVariantMap device{{"address", address}, {"name", "Mock Floss headset"}};
    uint bond = 0;
    bool connected = false, discovering = false;
    void spoofCallback() {
        const auto impostor = QDBusConnection::connectToBus(QDBusConnection::SystemBus, "test-impostor");
        auto message = QDBusMessage::createMethodCall(destination, callbackPath,
            "org.chromium.bluetooth.BluetoothCallback", "OnDeviceFound");
        message.setArguments({QVariantMap{{"address", "00:00:00:00:00:01"}, {"name", "Impostor"}}});
        auto *watcher = new QDBusPendingCallWatcher(impostor.asyncCall(message), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [watcher]() {
            const auto reply = watcher->reply();
            qInfo().noquote() << (reply.type() == QDBusMessage::ErrorMessage
                && reply.errorName() == "org.freedesktop.DBus.Error.AccessDenied" ? "SPOOF_REJECTED" : "BAD_SPOOF_ACCEPTED");
            watcher->deleteLater();
        });
        auto pairingMessage = QDBusMessage::createMethodCall(destination, callbackPath,
            "org.chromium.bluetooth.BluetoothCallback", "OnSspRequest");
        pairingMessage.setArguments({device, uint(0), uint(0), uint(654321)});
        auto *pairingWatcher = new QDBusPendingCallWatcher(impostor.asyncCall(pairingMessage), this);
        connect(pairingWatcher, &QDBusPendingCallWatcher::finished, this, [pairingWatcher]() {
            const auto reply = pairingWatcher->reply();
            qInfo().noquote() << (reply.type() == QDBusMessage::ErrorMessage
                && reply.errorName() == "org.freedesktop.DBus.Error.AccessDenied"
                ? "PAIRING_SPOOF_REJECTED" : "BAD_PAIRING_SPOOF_ACCEPTED");
            pairingWatcher->deleteLater();
        });
    }
    void callback(const QString &method, const QVariantList &args, bool connection = false) {
        const auto &callbacks = connection ? connectionCallbacks : adapterCallbacks;
        for (auto it = callbacks.cbegin(); it != callbacks.cend(); ++it) {
            auto message = QDBusMessage::createMethodCall(it.key(), it.value(),
                connection ? "org.chromium.bluetooth.BluetoothConnectionCallback" : "org.chromium.bluetooth.BluetoothCallback", method);
            message.setArguments(args); bus.asyncCall(message);
        }
        qInfo().noquote() << "CALLBACK" << method;
    }
public:
    Mock() {
        elapsed.start(); currentBattery = batterySet(73); if (reconnectScenario) bond = 2;
        const auto scenario = qEnvironmentVariable("FLOSS_MOCK_SCENARIO");
        if (scenario.startsWith("rfkill-")) {
            rfkillMode = scenario.mid(7);
            softBlocked = rfkillMode == "soft" || rfkillMode == "fail" || rfkillMode == "stuck";
        }
    }
    QString introspect(const QString &) const override { return {}; }
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override {
        const auto method = message.member();
        const auto args = message.arguments();
        qInfo().noquote() << "METHOD" << method << message.signature() << "AT" << elapsed.elapsed();
        QVariantList reply;
        QString expected;
        if (message.path() == "/org/chromium/bluetooth/Test"
                && message.interface() == "org.chromium.bluetooth.Test") {
            if (method == "TriggerPairing") {
                bond = 1;
                callback("OnBondStateChanged", {uint(0), address, uint(1)});
                callback("OnSspRequest", {device, uint(0), uint(0), uint(123456)});
            } else if (method == "SetRfkillMode") {
                expected = "s"; rfkillMode = args.value(0).toString();
                softBlocked = rfkillMode == "soft" || rfkillMode == "fail" || rfkillMode == "stuck";
            } else if (method == "GetPowerStatus") {
                reply = {starts, stops, unblocks};
            } else if (method == "GetStatus") {
                reply = {uint(adapterCallbacks.size()), pairingAnswers, pairingCancellations};
            } else return false;
        } else if (message.path() == "/org/chromium/bluetooth/Manager" && message.interface() == "org.chromium.bluetooth.Manager"
                && (method == "GetRfkillState" || method == "SetRfkillBlocked")) {
            expected = method == "GetRfkillState" ? "i" : "ib";
            if (args.value(0).toInt() != 0) qCritical() << "BAD_RFKILL_ADAPTER";
            if (method == "GetRfkillState") {
                reply = {QVariantMap{{"available", rfkillMode != "absent"},
                    {"hard_blocked", rfkillMode == "hard"}, {"soft_blocked", softBlocked}}};
            } else {
                if (args.value(1).toBool()) qCritical() << "BAD_UNEXPECTED_BLOCK";
                ++unblocks;
                if (rfkillMode != "fail" && rfkillMode != "stuck") softBlocked = false;
                reply = {rfkillMode != "fail"};
            }
        } else if (message.path() == "/org/chromium/bluetooth/Manager" && message.interface() == "org.chromium.bluetooth.Manager"
                && (method == "Start" || method == "Stop")) {
            expected = "i";
            if (method == "Start") {
                ++starts;
                if (rfkillMode == "hard" || softBlocked) qCritical() << "BAD_START_WHILE_BLOCKED";
                bus.registerService("org.chromium.bluetooth");
            } else ++stops;
            if (method == "Stop") QTimer::singleShot(100, this, [this]() {
                bus.unregisterService("org.chromium.bluetooth");
                qInfo() << "MOCK_ADAPTER_OWNER_REMOVED";
            });
        } else if (message.path() == "/org/chromium/bluetooth/hci0/battery_manager"
                   && message.interface() == "org.chromium.bluetooth.BatteryManager") {
            if (method == "RegisterBatteryCallback") {
                expected = "o"; batteryPath = qvariant_cast<QDBusObjectPath>(args.value(0)).path();
                batteryCallbacks[message.service()] = batteryPath; reply = {uint(1)};
                const auto target = message.service(); const auto path = batteryPath;
                QTimer::singleShot(100, this, [this, target, path]() {
                    auto impostor = QDBusConnection::connectToBus(QDBusConnection::SystemBus, "battery-impostor");
                    auto update = QDBusMessage::createMethodCall(target, path,
                        "org.chromium.bluetooth.BatteryManagerCallback", "OnBatteryInfoUpdated");
                    update.setArguments({address, batterySet(99)});
                    auto *watcher = new QDBusPendingCallWatcher(impostor.asyncCall(update), this);
                    connect(watcher, &QDBusPendingCallWatcher::finished, this, [watcher]() {
                        const auto reply = watcher->reply();
                        qInfo() << (reply.type() == QDBusMessage::ErrorMessage
                            && reply.errorName() == "org.freedesktop.DBus.Error.AccessDenied"
                            ? "BATTERY_SPOOF_REJECTED" : "BAD_BATTERY_SPOOF_ACCEPTED");
                        watcher->deleteLater();
                    });
                });
            } else if (method == "UnregisterBatteryCallback") {
                expected = "u"; reply = {true};
            } else if (method == "GetBatteryInformation") {
                expected = "s"; reply = {currentBattery.isEmpty() ? QVariantMap{} : QVariantMap{{"optional_value", currentBattery}}};
            } else return false;
        } else if (message.path() == "/org/chromium/bluetooth/hci0/adapter" && message.interface() == "org.chromium.bluetooth.Bluetooth") {
            if (method == "RegisterCallback" || method == "RegisterConnectionCallback") {
                expected = "o";
                destination = message.service();
                auto path = qvariant_cast<QDBusObjectPath>(args.value(0)).path();
                if (method == "RegisterCallback") { callbackPath = path; adapterCallbacks[destination] = path; }
                else { connectionPath = path; connectionCallbacks[destination] = path; }
                reply = {uint(1)};
                if (method == "RegisterConnectionCallback") QTimer::singleShot(10, this, [this]() { spoofCallback(); });
            } else if (method == "UnregisterCallback" || method == "UnregisterConnectionCallback") {
                expected = "u"; reply = {true};
            } else if (method == "GetBondedDevices" || method == "GetConnectedDevices") {
                reply = {QVariant::fromValue((method == "GetBondedDevices" ? bond == 2 : connected) ? DeviceList{device} : DeviceList{})};
            } else if (method == "IsDiscovering") {
                reply = {discovering};
            } else if (method == "StartDiscovery" || method == "CancelDiscovery") {
                discovering = method == "StartDiscovery"; reply = {true};
                QTimer::singleShot(30, this, [this]() {
                    callback("OnDiscoveringChanged", {discovering});
                    if (discovering) callback("OnDeviceFound", {device});
                });
            } else if (method == "GetBondState") {
                expected = "a{sv}"; reply = {bond};
            } else if (method == "GetRemoteAlias") {
                expected = "a{sv}"; reply = {alias};
            } else if (method == "SetRemoteAlias") {
                expected = "a{sv}s";
                if (args.value(1).toString() == "Rejected alias") {
                    connection.send(message.createErrorReply("org.freedesktop.DBus.Error.Failed", "Mock alias rejection")); return true;
                }
                if (args.value(1).toString() == "Ignored alias") {
                    connection.send(message.createReply()); return true;
                }
                alias = args.value(1).toString();
                QTimer::singleShot(30, this, [this]() {
                    callback("OnDevicePropertiesChanged", {device, QVariant::fromValue(QList<uint>{1})});
                    auto update = QDBusMessage::createMethodCall(destination, batteryPath,
                        "org.chromium.bluetooth.BatteryManagerCallback", "OnBatteryInfoUpdated");
                    auto info = batterySet(alias == "Zero battery" ? 0 : 61);
                    if (alias == "Multiple batteries") info["batteries"] = QVariant::fromValue(DeviceList{
                        QVariantMap{{"percentage", uint(54)}, {"variant", "left"}},
                        QVariantMap{{"percentage", uint(32)}, {"variant", "right"}}});
                    if (alias == "Invalid battery") info["batteries"] = QVariant::fromValue(DeviceList{
                        QVariantMap{{"percentage", uint(101)}, {"variant", ""}}});
                    if (alias == "Absent battery") info.clear();
                    currentBattery = info;
                    if (!info.isEmpty()) { update.setArguments({address, info}); bus.asyncCall(update); }
                    qInfo() << "BATTERY_UPDATED";
                });
            } else if (method == "GetRemoteConnected") {
                expected = "a{sv}"; reply = {connected};
            } else if (method == "CreateBond") {
                expected = "a{sv}u"; reply = {uint(0)}; bond = 1;
                QTimer::singleShot(30, this, [this]() {
                    callback("OnBondStateChanged", {uint(0), address, uint(1)});
                    callback("OnSspRequest", {device, uint(0), uint(0), uint(123456)});
                });
            } else if (method == "SetPairingConfirmation") {
                expected = "a{sv}b"; reply = {true}; bond = 2;
                ++pairingAnswers;
                if (pairingAnswers > 1) qCritical() << "BAD_DUPLICATE_PAIRING_ANSWER";
                QTimer::singleShot(30, this, [this]() { callback("OnBondStateChanged", {uint(0), address, uint(2)}); });
            } else if (method == "CancelBondProcess") {
                expected = "a{sv}"; reply = {true}; bond = 0;
                ++pairingCancellations;
                QTimer::singleShot(30, this, [this]() { callback("OnBondStateChanged", {uint(0), address, uint(0)}); });
            } else if (method == "ConnectAllEnabledProfiles" || method == "DisconnectAllEnabledProfiles") {
                expected = "a{sv}";
                const bool connecting = method == "ConnectAllEnabledProfiles";
                if (connecting && reconnectScenario) {
                    ++connectAttempts;
                    if (explicitDisconnect) qCritical() << "BAD_RECONNECT_AFTER_DISCONNECT";
                    if (connectAttempts == 1) {
                        reply = {uint(1)};
                        qInfo() << "MOCK_CONNECTION_REJECTED";
                        QTimer::singleShot(30, this, [this]() { callback("OnDeviceConnectionFailed", {device, uint(1)}, true); });
                        connection.send(message.createReply(reply));
                        return true;
                    }
                }
                if (!connecting) explicitDisconnect = true;
                reply = connecting ? QVariantList{uint(0)} : QVariantList{true};
                QTimer::singleShot(30, this, [this, connecting]() {
                    connected = connecting;
                    callback(connected ? "OnDeviceConnected" : "OnDeviceDisconnected", {device}, true);
                    if (reconnectScenario && !connecting && !lateConnectionSent) {
                        lateConnectionSent = true;
                        QTimer::singleShot(900, this, [this]() {
                            connected = true; callback("OnDeviceConnected", {device}, true);
                            qInfo() << "MOCK_LATE_CONNECTION";
                        });
                    } else if (reconnectScenario && !connecting && lateConnectionSent) {
                        qInfo() << "MOCK_LATE_CONNECTION_RECONCILED";
                    }
                    if (!reconnectScenario || !connecting) return;
                    ++connections;
                    if (connections == 3) {
                        QFile complete(qEnvironmentVariable("FLOSS_MOCK_READY") + "-reconnected");
                        if (complete.open(QIODevice::WriteOnly)) complete.write("ready");
                        qInfo() << "MOCK_HEADLESS_RECOVERY_COMPLETE";
                    }
                    if (connections == 1) QTimer::singleShot(1200, this, [this]() {
                        connected = false;
                        callback("OnDeviceDisconnected", {device}, true);
                        qInfo() << "MOCK_LINK_DROPPED";
                    });
                    if (connections == 2) QTimer::singleShot(1200, this, [this]() {
                        connected = false;
                        bus.unregisterService("org.chromium.bluetooth");
                        qInfo() << "MOCK_DAEMON_RESTART_BEGIN";
                        QTimer::singleShot(1200, this, [this]() {
                            bus.registerService("org.chromium.bluetooth");
                            qInfo() << "MOCK_DAEMON_RESTART_END";
                        });
                    });
                });
            } else if (method == "RemoveBond") {
                expected = "a{sv}"; reply = {true}; bond = 0;
                QTimer::singleShot(30, this, [this]() { callback("OnBondStateChanged", {uint(0), address, uint(0)}); });
            } else return false;
        } else return false;
        if (message.signature() != expected || (!expected.isEmpty() && expected == "i" && args[0].toInt() != 0)) {
            qCritical() << "BAD_SIGNATURE_OR_ADAPTER" << method << message.signature();
            connection.send(message.createErrorReply("org.freedesktop.DBus.Error.InvalidArgs", "Unexpected mock contract"));
            return true;
        }
        if (expected.startsWith("a{sv}") && qdbus_cast<QVariantMap>(args[0]).value("address").toString() != address) {
            qCritical() << "BAD_DEVICE";
            connection.send(message.createErrorReply("org.freedesktop.DBus.Error.InvalidArgs", "Unexpected device"));
            return true;
        }
        connection.send(message.createReply(reply));
        return true;
    }
};

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto address = qgetenv("DBUS_SYSTEM_BUS_ADDRESS");
    if (!address.contains("/tmp/floss-kde-smoke-") || address != qgetenv("DBUS_SESSION_BUS_ADDRESS")) return 2;
    qDBusRegisterMetaType<DeviceList>();
    qDBusRegisterMetaType<QList<uint>>();
    Mock mock;
    auto bus = QDBusConnection::systemBus();
    if (!bus.registerService("org.chromium.bluetooth") || !bus.registerService("org.chromium.bluetooth.Manager")
        || !bus.registerVirtualObject("/org/chromium/bluetooth", &mock, QDBusConnection::SubPath)) return 3;
    qInfo() << "MOCK_READY";
    QFile ready(qEnvironmentVariable("FLOSS_MOCK_READY"));
    if (!ready.open(QIODevice::WriteOnly)) return 4;
    ready.write("ready"); ready.close();
    QTimer::singleShot(180000, &app, &QCoreApplication::quit);
    return app.exec();
}
