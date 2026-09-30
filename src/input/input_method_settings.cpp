// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "input/input_method_settings.h"

#include <algorithm>

#include <QDBusConnection>
#include <QDBusError>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QSet>
#include <QStandardPaths>

#include "core/preferences.h"

namespace cutefish::input {
namespace {
constexpr auto service = "com.cutefish.InputMethod1";
constexpr auto path = "/com/cutefish/InputMethod1";
constexpr auto selectionKey = "inputMethod";

QString unescape(const QString &value)
{
    QString result;
    result.reserve(value.size());
    for (qsizetype i = 0; i < value.size(); ++i) {
        const QChar c = value.at(i);
        if (c != QLatin1Char('\\') || i + 1 == value.size()) {
            result += c;
            continue;
        }
        switch (value.at(++i).unicode()) {
        case 's':
            result += QLatin1Char(' ');
            break;
        case 'n':
            result += QLatin1Char('\n');
            break;
        case 't':
            result += QLatin1Char('\t');
            break;
        case 'r':
            result += QLatin1Char('\r');
            break;
        case '\\':
            result += QLatin1Char('\\');
            break;
        default:
            result += QLatin1Char('\\');
            result += value.at(i);
            break;
        }
    }
    return result;
}

QMap<QString, QString> readDesktopEntry(const QString &fileName)
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    QMap<QString, QString> entry;
    bool inGroup = false;
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        if (line.startsWith(QLatin1Char('['))) {
            if (inGroup)
                break;
            inGroup = line == QLatin1String("[Desktop Entry]");
            continue;
        }
        const auto separator = line.indexOf(QLatin1Char('='));
        if (!inGroup || separator <= 0)
            continue;
        const QString key = line.left(separator).trimmed();
        if (!key.contains(QLatin1Char('[')) && !entry.contains(key))
            entry.insert(key, line.mid(separator + 1).trimmed());
    }
    return entry;
}

QString resolveExecutable(const QString &program)
{
    if (!QFileInfo(program).isAbsolute())
        return QStandardPaths::findExecutable(program);
    const QFileInfo info(program);
    return info.isFile() && info.isExecutable() ? program : QString();
}
} // namespace

QStringList parseDesktopExec(const QString &exec)
{
    const QString value = unescape(exec);
    QStringList arguments;
    QString current;
    bool quoted = false;
    bool pending = false;
    for (qsizetype i = 0; i < value.size(); ++i) {
        const QChar c = value.at(i);
        if (quoted) {
            if (c == QLatin1Char('\\') && i + 1 < value.size()
                && QStringLiteral("\"`$\\").contains(value.at(i + 1))) {
                current += value.at(++i);
            } else if (c == QLatin1Char('"')) {
                quoted = false;
            } else {
                current += c;
            }
        } else if (c == QLatin1Char('"')) {
            quoted = pending = true;
        } else if (c.isSpace()) {
            if (pending || !current.isEmpty())
                arguments << std::exchange(current, {});
            pending = false;
        } else {
            current += c;
        }
    }
    if (quoted)
        return {};
    if (pending || !current.isEmpty())
        arguments << current;

    QStringList command;
    for (auto argument : std::as_const(arguments)) {
        // An input method is started without files or URLs, so field codes expand to nothing.
        if (argument.size() == 2 && argument.at(0) == QLatin1Char('%')
            && argument.at(1) != QLatin1Char('%'))
            continue;
        command << argument.replace(QStringLiteral("%%"), QStringLiteral("%"));
    }
    return command;
}

QList<InputMethodProvider> findInputMethodProviders(const QStringList &applicationDirs)
{
    QList<InputMethodProvider> providers;
    QSet<QString> seen;
    for (const auto &directory : applicationDirs) {
        const QDir root(directory);
        QStringList files;
        QDirIterator it(directory, {QStringLiteral("*.desktop")}, QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext())
            files << it.next();
        files.sort();
        for (const auto &file : std::as_const(files)) {
            const QString id =
                    root.relativeFilePath(file).replace(QLatin1Char('/'), QLatin1Char('-'));
            // An earlier directory shadows the id even when its entry is not a provider.
            if (seen.contains(id))
                continue;
            seen.insert(id);
            const auto entry = readDesktopEntry(file);
            if (entry.value(QStringLiteral("X-KDE-Wayland-VirtualKeyboard"))
                        != QLatin1String("true")
                || entry.value(QStringLiteral("Type"), QStringLiteral("Application"))
                        != QLatin1String("Application")
                || entry.value(QStringLiteral("Hidden")) == QLatin1String("true"))
                continue;
            const QString tryExec = unescape(entry.value(QStringLiteral("TryExec")));
            if (!tryExec.isEmpty() && resolveExecutable(tryExec).isEmpty())
                continue;
            auto command = parseDesktopExec(entry.value(QStringLiteral("Exec")));
            if (command.isEmpty())
                continue;
            command.first() = resolveExecutable(command.first());
            if (command.first().isEmpty())
                continue;
            providers.append({id, file, command});
        }
    }
    std::sort(providers.begin(), providers.end(), [](const auto &a, const auto &b) {
        return a.id < b.id;
    });
    return providers;
}

InputMethodSettings::InputMethodSettings(QObject *parent, const QString &settingsFile,
                                         const QStringList &applicationDirs)
    : QObject(parent)
    , m_settings(settingsFile.isEmpty() ? preferencesFile(QStringLiteral("compositor"))
                                       : settingsFile,
                 QSettings::IniFormat)
    , m_applicationDirs(applicationDirs.isEmpty() ? QStandardPaths::standardLocations(
                                                            QStandardPaths::ApplicationsLocation)
                                                  : applicationDirs)
{
    m_settings.beginGroup(QStringLiteral("InputMethod"));
    m_selected = m_settings.value(QLatin1String(selectionKey)).toString();
}

InputMethodSettings::~InputMethodSettings()
{
    if (m_registered) {
        auto bus = QDBusConnection::sessionBus();
        bus.unregisterObject(QLatin1String(path));
        bus.unregisterService(QLatin1String(service));
    }
}

bool InputMethodSettings::registerService()
{
    if (m_registered)
        return true;
    auto bus = QDBusConnection::sessionBus();
    if (!bus.registerObject(QLatin1String(path), this,
                            QDBusConnection::ExportAllProperties | QDBusConnection::ExportAllSlots
                                    | QDBusConnection::ExportAllSignals)) {
        qWarning() << "Cannot export input method settings:" << bus.lastError().message();
        return false;
    }
    if (!bus.registerService(QLatin1String(service))) {
        qWarning() << "Cannot register input method settings:" << bus.lastError().message();
        bus.unregisterObject(QLatin1String(path));
        return false;
    }
    m_registered = true;
    return true;
}

std::optional<InputMethodProvider> InputMethodSettings::provider(const QString &id) const
{
    if (id.isEmpty())
        return std::nullopt;
    for (const auto &provider : findInputMethodProviders(m_applicationDirs)) {
        if (provider.id == id)
            return provider;
    }
    return std::nullopt;
}

QString InputMethodSettings::inputMethod() const
{
    return provider(m_selected) ? m_selected : QString();
}

QStringList InputMethodSettings::command() const
{
    const auto selected = provider(m_selected);
    return selected ? selected->command : QStringList();
}

void InputMethodSettings::setRunning(bool running)
{
    if (m_running == running)
        return;
    m_running = running;
    emit runningChanged();
}

void InputMethodSettings::overrideInputMethod(const QString &id)
{
    m_selected = id;
    emit inputMethodChanged();
    emit commandChanged();
}

QStringList InputMethodSettings::availableInputMethods() const
{
    QStringList ids;
    for (const auto &provider : findInputMethodProviders(m_applicationDirs))
        ids << provider.id;
    return ids;
}

bool InputMethodSettings::setInputMethod(const QString &id)
{
    if (!id.isEmpty() && !provider(id))
        return false;
    const bool changed = inputMethod() != id;
    m_selected = id;
    m_settings.setValue(QLatin1String(selectionKey), id);
    m_settings.sync();
    if (m_settings.status() != QSettings::NoError)
        qWarning() << "Cannot persist input method settings to" << m_settings.fileName();
    if (changed)
        emit inputMethodChanged();
    // Also sent for an unchanged selection, so selecting it again restarts a stopped method.
    emit commandChanged();
    return true;
}

} // namespace cutefish::input
