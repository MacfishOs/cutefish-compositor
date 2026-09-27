// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDBusPendingCallWatcher>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include <optional>

#include "display_config_interface.h"
#include "ipc/display_dbus_types.h"

namespace ipc = cutefish::display::ipc;

namespace {

QJsonObject modeJson(const ipc::Mode &mode)
{
    return {{QStringLiteral("width"), mode.width},
            {QStringLiteral("height"), mode.height},
            {QStringLiteral("refreshMilliHz"), mode.refreshMilliHz},
            {QStringLiteral("name"), mode.name},
            {QStringLiteral("preferred"), mode.preferred},
            {QStringLiteral("current"), mode.current}};
}

void printState(const QDBusMessage &reply)
{
    const auto values = reply.arguments();
    if (values.size() != 9)
        return;
    QJsonArray physical;
    for (const auto &display : qdbus_cast<QList<ipc::PhysicalDisplay>>(values.at(4))) {
        QJsonArray modes;
        for (const auto &mode : display.modes)
            modes.append(modeJson(mode));
        physical.append(QJsonObject {{QStringLiteral("id"), display.sessionId},
                                     {QStringLiteral("persistentKey"), display.persistentKey},
                                     {QStringLiteral("connector"), display.connectorName},
                                     {QStringLiteral("make"), display.make},
                                     {QStringLiteral("model"), display.model},
                                     {QStringLiteral("modes"), modes}});
    }
    QJsonArray logical;
    for (const auto &display : qdbus_cast<QList<ipc::LogicalDisplay>>(values.at(5))) {
        logical.append(QJsonObject {{QStringLiteral("id"), display.id},
                                    {QStringLiteral("name"), display.waylandName},
                                    {QStringLiteral("x"), display.x},
                                    {QStringLiteral("y"), display.y},
                                    {QStringLiteral("width"), display.width},
                                    {QStringLiteral("height"), display.height},
                                    {QStringLiteral("scale120"), int(display.scale120)},
                                    {QStringLiteral("rotation"), int(display.rotation)},
                                    {QStringLiteral("primary"), display.primary}});
    }
    const QJsonObject root {
            {QStringLiteral("instanceId"), values.at(0).toString()},
            {QStringLiteral("seatId"), values.at(1).toString()},
            {QStringLiteral("topologyGeneration"), QString::number(values.at(2).toULongLong())},
            {QStringLiteral("appliedRevision"), QString::number(values.at(3).toULongLong())},
            {QStringLiteral("physicalDisplays"), physical},
            {QStringLiteral("logicalDisplays"), logical},
            {QStringLiteral("previewToken"), values.at(7).toString()},
            {QStringLiteral("previewDeadlineMs"), QString::number(values.at(8).toULongLong())}};
    printf("%s", QJsonDocument(root).toJson(QJsonDocument::Indented).constData());
}

bool readConfiguration(const QString &path, QString *instanceId, qulonglong *generation,
                       qulonglong *revision, QList<ipc::DisplayRequest> *displays, QString *primary,
                       QList<ipc::Placement> *placements, ipc::MirrorGroups *mirrorGroups)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    QJsonParseError error;
    const auto root = QJsonDocument::fromJson(file.readAll(), &error).object();
    if (error.error != QJsonParseError::NoError)
        return false;
    *instanceId = root.value(QStringLiteral("instanceId")).toString();
    *generation = root.value(QStringLiteral("topologyGeneration")).toVariant().toULongLong();
    *revision = root.value(QStringLiteral("appliedRevision")).toVariant().toULongLong();
    *primary = root.value(QStringLiteral("primary")).toString();
    for (const auto &value : root.value(QStringLiteral("displays")).toArray()) {
        const auto item = value.toObject();
        const auto mode = item.value(QStringLiteral("mode")).toObject();
        ipc::DisplayRequest request;
        request.id = item.value(QStringLiteral("id")).toString();
        request.enabled = item.value(QStringLiteral("enabled")).toBool(true);
        request.scale120 = item.value(QStringLiteral("scale120")).toInt(120);
        request.rotation = item.value(QStringLiteral("rotation")).toInt();
        request.mode = {mode.value(QStringLiteral("width")).toInt(),
                        mode.value(QStringLiteral("height")).toInt(),
                        mode.value(QStringLiteral("refreshMilliHz")).toInt(),
                        mode.value(QStringLiteral("pixelClockKHz")).toInt(),
                        mode.value(QStringLiteral("horizontalTotal")).toInt(),
                        mode.value(QStringLiteral("verticalTotal")).toInt(),
                        uint(mode.value(QStringLiteral("flags")).toInt()),
                        mode.value(QStringLiteral("preferred")).toBool(),
                        mode.value(QStringLiteral("name")).toString()};
        displays->append(request);
    }
    for (const auto &value : root.value(QStringLiteral("placements")).toArray()) {
        const auto item = value.toObject();
        placements->append({item.value(QStringLiteral("display")).toString(),
                            item.value(QStringLiteral("parent")).toString(),
                            uint(item.value(QStringLiteral("edge")).toInt()),
                            item.value(QStringLiteral("offset")).toInt()});
    }
    for (const auto &value : root.value(QStringLiteral("mirrorGroups")).toArray()) {
        QStringList group;
        for (const auto &id : value.toArray())
            group.append(id.toString());
        mirrorGroups->append(group);
    }
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("service"), QStringLiteral("D-Bus service name"),
                      QStringLiteral("name"), QStringLiteral("com.cutefish.DisplayConfig")});
    parser.addPositionalArgument(
            QStringLiteral("command"),
            QStringLiteral("list, test, apply, confirm, revert, reset, or power"));
    parser.addPositionalArgument(QStringLiteral("arguments"), QStringLiteral("Command arguments"),
                                 QStringLiteral("[arguments...]"));
    parser.process(app);
    const auto args = parser.positionalArguments();
    if (args.isEmpty())
        parser.showHelp(2);

    ipc::registerTypes();
    ComCutefishDisplayConfig1Interface service(parser.value(QStringLiteral("service")),
                                               QStringLiteral("/com/cutefish/DisplayConfig1"),
                                               QDBusConnection::sessionBus());
    if (!service.isValid()) {
        qCritical("Display configuration service is unavailable");
        return 1;
    }

    const QString command = args.first();
    QString powerToken;
    QHash<QString, QPair<bool, QString>> powerResults;
    const auto finishPower = [&app](const QPair<bool, QString> &result) {
        if (!result.first)
            qCritical().noquote() << "Display power operation failed:" << result.second;
        app.exit(result.first ? 0 : 1);
    };
    if (command == QStringLiteral("power")) {
        QObject::connect(&service, &ComCutefishDisplayConfig1Interface::OperationFinished, &app,
                         [&](const QString &token, bool succeeded, bool, uint,
                             const QString &message, const QStringList &, qulonglong, qulonglong) {
                             if (powerToken.isEmpty())
                                 powerResults.insert(token, {succeeded, message});
                             else if (powerToken == token)
                                 finishPower({succeeded, message});
                         });
        QTimer::singleShot(15000, &app, [&app] {
            qCritical("Timed out waiting for display power operation");
            app.exit(1);
        });
    }
    std::optional<QDBusPendingCall> call;
    if (command == QStringLiteral("list")) {
        call.emplace(service.GetState());
    } else if ((command == QStringLiteral("test") || command == QStringLiteral("apply"))
               && args.size() == 2) {
        QString instanceId, primary;
        qulonglong generation = 0, revision = 0;
        QList<ipc::DisplayRequest> displays;
        QList<ipc::Placement> placements;
        ipc::MirrorGroups mirrors;
        if (!readConfiguration(args.at(1), &instanceId, &generation, &revision, &displays, &primary,
                               &placements, &mirrors)) {
            qCritical("Could not read the configuration JSON");
            return 2;
        }
        if (command == QStringLiteral("test"))
            call.emplace(service.TestConfiguration(instanceId, generation, revision, displays,
                                                   primary, placements, mirrors));
        else
            call.emplace(service.ApplyConfiguration(instanceId, generation, revision, displays,
                                                    primary, placements, mirrors));
    } else if (command == QStringLiteral("confirm") && args.size() == 2) {
        call.emplace(service.ConfirmConfiguration(args.at(1)));
    } else if (command == QStringLiteral("revert") && args.size() == 2) {
        call.emplace(service.RevertConfiguration(args.at(1)));
    } else if (command == QStringLiteral("power") && args.size() >= 2) {
        const QString state = args.at(1);
        const uint value = state == QStringLiteral("on") ? 0
                : state == QStringLiteral("off")         ? 1
                : state == QStringLiteral("suspend")     ? 2
                                                         : 99;
        if (value == 99) {
            qCritical("Power state must be on, off, or suspend");
            return 2;
        }
        call.emplace(service.SetDisplayPower(args.mid(2), value));
    } else {
        qCritical("Invalid command or arguments");
        return 2;
    }

    auto *watcher = new QDBusPendingCallWatcher(*call, &app);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, &app,
                     [watcher, command, &app, &powerToken, &powerResults, &finishPower] {
                         const QDBusMessage reply = watcher->reply();
                         if (reply.type() == QDBusMessage::ErrorMessage) {
                             qCritical().noquote() << reply.errorName() << reply.errorMessage();
                             app.exit(1);
                         } else if (command == QStringLiteral("power")) {
                             powerToken = reply.arguments().value(0).toString();
                             if (powerToken.isEmpty()) {
                                 qCritical("Display power request returned no operation token");
                                 app.exit(1);
                             } else if (powerResults.contains(powerToken)) {
                                 finishPower(powerResults.take(powerToken));
                             }
                         } else {
                             if (command == QStringLiteral("list"))
                                 printState(reply);
                             else if (!reply.arguments().isEmpty())
                                 printf("%s\n", qPrintable(reply.arguments().first().toString()));
                             app.exit(0);
                         }
                         watcher->deleteLater();
                     });
    return app.exec();
}
