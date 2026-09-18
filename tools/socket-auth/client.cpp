// Real-daemon authorization probe. Run only on the smoke runner's private bus.
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QVariantMap>
#include <cstdio>
#include <memory>
#include <stdexcept>

static QDBusMessage call(const QDBusConnection &bus, const QString &method, const QVariantList &args)
{
    auto message = QDBusMessage::createMethodCall("org.chromium.bluetooth",
        "/org/chromium/bluetooth/hci0/adapter", "org.chromium.bluetooth.SocketManager", method);
    message.setArguments(args);
    return bus.call(message, QDBus::Block, 2000);
}
static void require(bool condition, const QString &description)
{
    if (!condition) throw std::runtime_error(description.toStdString());
}
static uint registerCallback(const QDBusConnection &bus)
{
    const auto reply = call(bus, "RegisterCallback", {QVariant::fromValue(QDBusObjectPath("/test/shared_socket_callback"))});
    require(reply.type() == QDBusMessage::ReplyMessage && reply.signature() == "u",
        "RegisterCallback failed: " + reply.errorName() + " " + reply.errorMessage());
    return reply.arguments().at(0).toUInt();
}
static void denied(const QDBusConnection &bus, const QString &method, const QVariantList &args)
{
    const auto reply = call(bus, method, args);
    require(reply.type() == QDBusMessage::ErrorMessage
        && reply.errorName() == "org.freedesktop.DBus.Error.AccessDenied",
        method + " did not deny foreign callback: " + reply.errorName() + " " + reply.signature());
}
static void ownMissingSocket(const QDBusConnection &bus, uint id)
{
    for (const auto &method : {QString("Accept"), QString("Close")}) {
        QVariantList args{id, QVariant::fromValue(qulonglong(0))};
        if (method == "Accept") args.append(QVariantMap{});
        const auto reply = call(bus, method, args);
        require(reply.type() == QDBusMessage::ReplyMessage && reply.signature() == "u"
            && reply.arguments().at(0).toUInt() != 0,
            method + " failed valid-owner dispatch to missing socket");
    }
}
int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    const auto address = qEnvironmentVariable("DBUS_SYSTEM_BUS_ADDRESS");
    if (address != "unix:path=/tmp/private-bus" || address != qEnvironmentVariable("DBUS_SESSION_BUS_ADDRESS")) return 2;
    QJsonObject report;
    try {
        auto first = QDBusConnection::connectToBus(address, "socket-auth-first");
        auto second = std::make_unique<QDBusConnection>(QDBusConnection::connectToBus(address, "socket-auth-second"));
        require(first.isConnected() && second->isConnected() && first.baseService() != second->baseService(), "distinct clients missing");
        const auto firstId = registerCallback(first), secondId = registerCallback(*second);
        require(firstId != secondId, "same object path aliased across owners");
        require(registerCallback(first) == firstId && registerCallback(*second) == secondId, "same-owner registration lost idempotency");
        report["ownerPathIsolation"] = true;
        QJsonArray methods;
        const QVariantMap device{{"address", "AA:BB:CC:DD:EE:FF"}, {"name", "Authorization test only"}};
        const QByteArray uuid(16, '\0');
        auto check = [&](const QString &method, QVariantList args) {
            args.prepend(firstId);
            denied(*second, method, args);
            methods.append(method);
        };
        check("UnregisterCallback", {});
        for (const auto &method : {"ListenUsingInsecureL2capChannel", "ListenUsingInsecureL2capLeChannel",
                                  "ListenUsingL2capChannel", "ListenUsingL2capLeChannel"}) check(method, {});
        for (const auto &method : {"ListenUsingInsecureRfcommWithServiceRecord", "ListenUsingRfcommWithServiceRecord"})
            check(method, {QString("Authorization test only"), uuid});
        check("ListenUsingRfcomm", {QVariantMap{}, QVariantMap{}, QVariantMap{}, QVariantMap{}});
        for (const auto &method : {"CreateInsecureL2capChannel", "CreateInsecureL2capLeChannel",
                                  "CreateL2capChannel", "CreateL2capLeChannel"}) check(method, {device, int(1)});
        for (const auto &method : {"CreateInsecureRfcommSocketToServiceRecord", "CreateRfcommSocketToServiceRecord"})
            check(method, {device, uuid});
        check("Accept", {QVariant::fromValue(qulonglong(0)), QVariantMap{}});
        check("Close", {QVariant::fromValue(qulonglong(0))});
        report["foreignMethodsDenied"] = methods;
        denied(*second, "CreateL2capChannel", {firstId, QVariantMap{}, int(1)});
        report["authorizationBeforeDeviceConversion"] = true;
        ownMissingSocket(first, firstId);
        ownMissingSocket(*second, secondId);
        report["validOwnerMissingSocketDispatch"] = true;
        auto spoof = QDBusMessage::createSignal("/org/freedesktop/DBus", "org.freedesktop.DBus", "NameOwnerChanged");
        spoof.setArguments({first.baseService(), first.baseService(), QString()});
        require(second->send(spoof), "could not send private-bus forged-disconnect probe");
        QThread::msleep(100);
        ownMissingSocket(first, firstId);
        report["forgedDisconnectIgnored"] = true;
        const auto removed = call(first, "UnregisterCallback", {firstId});
        require(removed.type() == QDBusMessage::ReplyMessage && removed.signature() == "b"
            && removed.arguments().at(0).toBool(), "owner unregister failed");
        denied(first, "UnregisterCallback", {firstId});
        ownMissingSocket(*second, secondId);
        const auto replacementId = registerCallback(first);
        require(replacementId != firstId && replacementId != secondId, "removed callback identity reused");
        report["unregisterIsolatedAndReplacementFresh"] = true;
        QDBusConnection::disconnectFromBus("socket-auth-second");
        second.reset();
        QThread::msleep(250);
        auto replacement = QDBusConnection::connectToBus(address, "socket-auth-new-second");
        const auto reconnectId = registerCallback(replacement);
        require(reconnectId != secondId && reconnectId != replacementId, "reconnected client inherited identity");
        denied(replacement, "Close", {secondId, QVariant::fromValue(qulonglong(0))});
        ownMissingSocket(first, replacementId);
        ownMissingSocket(replacement, reconnectId);
        report["disconnectedIdentityNotReusable"] = true;
        report["passed"] = true;
    } catch (const std::exception &error) {
        report["passed"] = false;
        report["error"] = QString::fromUtf8(error.what());
    }
    const auto json = QJsonDocument(report).toJson(QJsonDocument::Compact);
    std::printf("%s\n", json.constData());
    return report["passed"].toBool() ? 0 : 1;
}
