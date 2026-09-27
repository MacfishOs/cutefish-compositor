// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ash/accelerators/ash_accelerator_configuration.cc
// Revision: 8abbac30b9cf2fe976422c58c000c94685648b65
//
// Adapted for Cutefish: integrates the upstream behavior with native
// compositor types and ownership.

#include "accelerators/accelerator_configuration.h"

#include <algorithm>

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include "core/logging.h"

namespace cutefish::accelerators {

namespace {

constexpr auto kOverridesGroup = "overrides";
constexpr auto kKeyField = "key";
constexpr auto kModifiersField = "modifiers";
constexpr auto kStateField = "state";
constexpr auto kActionField = "action";

QString preferenceFile()
{
    QSettings settings(QSettings::UserScope, QStringLiteral("cutefish"),
                       QStringLiteral("shortcuts"));
    return settings.fileName();
}

const std::array<std::pair<uint32_t, const char *>, 5> kModifierNames = {{
        {ControlDown, "Ctrl"},
        {AltDown, "Alt"},
        {ShiftDown, "Shift"},
        {MetaDown, "Meta"},
        {AltGrDown, "AltGr"},
}};

QJsonArray modifiersToJson(uint32_t modifiers)
{
    QJsonArray names;
    for (const auto &[flag, name] : kModifierNames) {
        if (modifiers & flag)
            names.append(QString::fromLatin1(name));
    }
    return names;
}

uint32_t modifiersFromJson(const QJsonArray &names)
{
    uint32_t modifiers = NoModifier;
    for (const QJsonValue &value : names) {
        for (const auto &[flag, name] : kModifierNames) {
            if (value.toString() == QLatin1String(name))
                modifiers |= flag;
        }
    }
    return modifiers;
}

QJsonObject modificationToJson(const Accelerator &accelerator,
                               AcceleratorConfiguration::ModificationAction action)
{
    QJsonObject object;
    object.insert(QLatin1String(kKeyField), keysymName(accelerator.keysym()));
    object.insert(QLatin1String(kModifiersField), modifiersToJson(accelerator.modifiers()));
    object.insert(QLatin1String(kStateField),
                  accelerator.keyState() == Accelerator::KeyState::Pressed
                          ? QStringLiteral("pressed")
                          : QStringLiteral("released"));
    object.insert(QLatin1String(kActionField),
                  action == AcceleratorConfiguration::ModificationAction::Add
                          ? QStringLiteral("add")
                          : QStringLiteral("remove"));
    return object;
}

struct Modification {
    Accelerator accelerator;
    AcceleratorConfiguration::ModificationAction action;
};

std::optional<Modification> modificationFromJson(const QJsonValue &value)
{
    if (!value.isObject())
        return std::nullopt;
    const QJsonObject object = value.toObject();
    const uint32_t keysym = keysymFromName(object.value(QLatin1String(kKeyField)).toString());
    if (keysym == 0)
        return std::nullopt;
    const QString action = object.value(QLatin1String(kActionField)).toString();
    if (action != QLatin1String("add") && action != QLatin1String("remove"))
        return std::nullopt;
    Accelerator accelerator(keysym, modifiersFromJson(object.value(QLatin1String(kModifiersField)).toArray()));
    if (object.value(QLatin1String(kStateField)).toString() == QLatin1String("released"))
        accelerator.setKeyState(Accelerator::KeyState::Released);
    return Modification {accelerator,
                         action == QLatin1String("add")
                                 ? AcceleratorConfiguration::ModificationAction::Add
                                 : AcceleratorConfiguration::ModificationAction::Remove};
}

void setLookupMaps(std::span<const AcceleratorData> accelerators,
                   ActionIdToAcceleratorsMap &idToAccelerator,
                   AcceleratorMap<AcceleratorAction> &acceleratorToId,
                   std::set<Accelerator> &lockedAccelerators)
{
    for (const AcceleratorData &data : accelerators) {
        Accelerator accelerator(data.keysym, data.modifiers);
        accelerator.setKeyState(data.triggerOnPress ? Accelerator::KeyState::Pressed
                                                    : Accelerator::KeyState::Released);
        if (!acceleratorToId.insertNew(accelerator, data.action)) {
            qCWarning(cfInput) << "Duplicate default accelerator" << accelerator.shortcutText();
            continue;
        }
        idToAccelerator[toActionId(data.action)].push_back(accelerator);
        if (data.acceleratorLocked)
            lockedAccelerators.insert(accelerator);
    }
}

} // namespace

const char *toString(AcceleratorConfigResult result)
{
    switch (result) {
    case AcceleratorConfigResult::Success:
        return "success";
    case AcceleratorConfigResult::ActionLocked:
        return "action-locked";
    case AcceleratorConfigResult::AcceleratorLocked:
        return "accelerator-locked";
    case AcceleratorConfigResult::Conflict:
        return "conflict";
    case AcceleratorConfigResult::ConflictCanOverride:
        return "conflict-can-override";
    case AcceleratorConfigResult::NotFound:
        return "not-found";
    case AcceleratorConfigResult::KeyNotAllowed:
        return "key-not-allowed";
    case AcceleratorConfigResult::MissingModifier:
        return "missing-modifier";
    case AcceleratorConfigResult::ShiftOnlyNotAllowed:
        return "shift-only-not-allowed";
    case AcceleratorConfigResult::MaximumAcceleratorsReached:
        return "maximum-accelerators-reached";
    case AcceleratorConfigResult::RestoreSuccessWithConflicts:
        return "restore-success-with-conflicts";
    case AcceleratorConfigResult::NonMetaAcceleratorWarning:
        return "non-meta-accelerator-warning";
    case AcceleratorConfigResult::ReservedKeyNotAllowed:
        return "reserved-key-not-allowed";
    }
    return "unknown";
}

AcceleratorConfiguration::AcceleratorConfiguration(const QString &settingsFile, QObject *parent)
    : QObject(parent)
    , m_settings(settingsFile.isEmpty() ? preferenceFile() : settingsFile, QSettings::IniFormat)
{
}

AcceleratorConfiguration::~AcceleratorConfiguration() = default;

void AcceleratorConfiguration::initialize()
{
    // The defaults' own update saves the override cache, so it must already
    // hold what the file has.
    if (customizable())
        loadOverrides();
    else
        m_overrides.clear();
    if (m_convention == input::ShortcutConvention::Command)
        initialize(kCommandAcceleratorData);
    else
        initialize(kAcceleratorData);
    if (customizable())
        applyOverrides();
}

bool AcceleratorConfiguration::customizable() const
{
    return m_convention == input::ShortcutConvention::Pc;
}

void AcceleratorConfiguration::setConvention(input::ShortcutConvention convention)
{
    if (m_convention == convention)
        return;
    m_convention = convention;
    initialize();
}

void AcceleratorConfiguration::initialize(std::span<const AcceleratorData> accelerators)
{
    m_accelerators.clear();
    m_idToAccelerators.clear();
    m_acceleratorToId.clear();
    m_defaultAcceleratorsToId.clear();
    m_defaultIdToAccelerators.clear();
    m_lockedAccelerators.clear();

    setLookupMaps(accelerators, m_defaultIdToAccelerators, m_defaultAcceleratorsToId,
                  m_lockedAccelerators);
    // Actions without a default are still actions the user can bind.
    for (const AcceleratorAction action : allAcceleratorActions()) {
        m_defaultIdToAccelerators.try_emplace(toActionId(action));
        m_idToAccelerators.try_emplace(toActionId(action));
    }
    addAccelerators(accelerators);
}

void AcceleratorConfiguration::addAccelerators(std::span<const AcceleratorData> accelerators)
{
    setLookupMaps(accelerators, m_idToAccelerators, m_acceleratorToId, m_lockedAccelerators);
    updateAndNotifyAccelerators();
}

std::optional<std::vector<Accelerator>>
AcceleratorConfiguration::acceleratorsForAction(AcceleratorActionId id) const
{
    const auto it = m_idToAccelerators.find(id);
    if (it == m_idToAccelerators.end())
        return std::nullopt;
    return it->second;
}

const AcceleratorAction *
AcceleratorConfiguration::findAcceleratorAction(const Accelerator &accelerator) const
{
    return m_acceleratorToId.find(accelerator);
}

bool AcceleratorConfiguration::isAcceleratorLocked(const Accelerator &accelerator) const
{
    return m_lockedAccelerators.contains(accelerator);
}

std::optional<AcceleratorAction>
AcceleratorConfiguration::idForDefaultAccelerator(const Accelerator &accelerator) const
{
    const AcceleratorAction *action = m_defaultAcceleratorsToId.find(accelerator);
    return action ? std::optional<AcceleratorAction>(*action) : std::nullopt;
}

std::vector<Accelerator>
AcceleratorConfiguration::defaultAcceleratorsForId(AcceleratorActionId id) const
{
    const auto it = m_defaultIdToAccelerators.find(id);
    return it == m_defaultIdToAccelerators.end() ? std::vector<Accelerator>() : it->second;
}

bool AcceleratorConfiguration::isValid(AcceleratorActionId id) const
{
    return m_idToAccelerators.contains(id) && m_defaultIdToAccelerators.contains(id);
}

bool AcceleratorConfiguration::hasCustomAccelerators() const
{
    return totalNumberOfModifications() > 0;
}

AcceleratorConfigResult AcceleratorConfiguration::addUserAccelerator(AcceleratorActionId id,
                                                                     const Accelerator &accelerator)
{
    if (!customizable())
        return AcceleratorConfigResult::ActionLocked;
    const AcceleratorConfigResult result = doAddAccelerator(id, accelerator, /*saveOverride=*/true);
    if (result == AcceleratorConfigResult::Success)
        updateAndNotifyAccelerators();
    qCDebug(cfInput) << "addAccelerator" << id << accelerator.shortcutText() << toString(result);
    return result;
}

AcceleratorConfigResult AcceleratorConfiguration::removeAccelerator(AcceleratorActionId id,
                                                                    const Accelerator &accelerator)
{
    if (!customizable())
        return AcceleratorConfigResult::ActionLocked;
    const AcceleratorConfigResult result =
            doRemoveAccelerator(id, accelerator, /*saveOverride=*/true);
    if (result == AcceleratorConfigResult::Success)
        updateAndNotifyAccelerators();
    qCDebug(cfInput) << "removeAccelerator" << id << accelerator.shortcutText() << toString(result);
    return result;
}

AcceleratorConfigResult
AcceleratorConfiguration::replaceAccelerator(AcceleratorActionId id,
                                             const Accelerator &oldAccelerator,
                                             const Accelerator &newAccelerator)
{
    if (!customizable())
        return AcceleratorConfigResult::ActionLocked;
    const AcceleratorConfigResult result = doReplaceAccelerator(id, oldAccelerator, newAccelerator);
    if (result == AcceleratorConfigResult::Success)
        updateAndNotifyAccelerators();
    qCDebug(cfInput) << "replaceAccelerator" << id << oldAccelerator.shortcutText() << "->"
                     << newAccelerator.shortcutText() << toString(result);
    return result;
}

AcceleratorConfigResult AcceleratorConfiguration::restoreDefault(AcceleratorActionId id)
{
    if (!customizable())
        return AcceleratorConfigResult::ActionLocked;
    const auto current = m_idToAccelerators.find(id);
    if (current == m_idToAccelerators.end())
        return AcceleratorConfigResult::NotFound;

    std::vector<Accelerator> &acceleratorsForId = current->second;
    // Clear the reverse mapping first.
    for (const Accelerator &accelerator : acceleratorsForId) {
        const AcceleratorAction *found = m_acceleratorToId.find(accelerator);
        if (!found || toActionId(*found) != id)
            return AcceleratorConfigResult::NotFound;
        m_acceleratorToId.erase(accelerator);
    }
    acceleratorsForId.clear();

    // Only bring back the defaults nobody else is using now. The user re-adds
    // the others by hand, which is what makes the conflict visible.
    const auto defaults = m_defaultIdToAccelerators.find(id);
    Q_ASSERT(defaults != m_defaultIdToAccelerators.end());

    AcceleratorConfigResult result = AcceleratorConfigResult::Success;
    for (const Accelerator &accelerator : defaults->second) {
        if (!m_acceleratorToId.find(accelerator)) {
            acceleratorsForId.push_back(accelerator);
            m_acceleratorToId.insertNew(accelerator, static_cast<AcceleratorAction>(id));
        } else {
            result = AcceleratorConfigResult::RestoreSuccessWithConflicts;
        }
    }

    m_overrides.erase(acceleratorActionName(static_cast<AcceleratorAction>(id)));
    updateAndNotifyAccelerators();
    qCDebug(cfInput) << "restoreDefault" << id << toString(result);
    return result;
}

AcceleratorConfigResult AcceleratorConfiguration::restoreAllDefaults()
{
    if (!customizable())
        return AcceleratorConfigResult::ActionLocked;
    resetAllAccelerators();
    m_overrides.clear();
    updateAndNotifyAccelerators();
    return AcceleratorConfigResult::Success;
}

AcceleratorConfigResult AcceleratorConfiguration::doRemoveAccelerator(
        AcceleratorActionId id, const Accelerator &accelerator, bool saveOverride)
{
    const AcceleratorAction *foundId = m_acceleratorToId.find(accelerator);
    const auto foundAccelerators = m_idToAccelerators.find(id);
    if (foundAccelerators == m_idToAccelerators.end() || !foundId)
        return AcceleratorConfigResult::NotFound;
    if (toActionId(*foundId) != id)
        return AcceleratorConfigResult::NotFound;

    std::erase(foundAccelerators->second, accelerator);
    m_acceleratorToId.erase(accelerator);

    // A press/release pair belongs together: removing one removes the other.
    Accelerator reverse(accelerator);
    reverse.setKeyState(accelerator.keyState() == Accelerator::KeyState::Pressed
                                ? Accelerator::KeyState::Released
                                : Accelerator::KeyState::Pressed);
    const AcceleratorAction *reverseId = m_acceleratorToId.find(reverse);
    if (reverseId && toActionId(*reverseId) == id) {
        std::erase(foundAccelerators->second, reverse);
        m_acceleratorToId.erase(reverse);
    }

    if (saveOverride)
        updateOverrides(id, accelerator, ModificationAction::Remove);
    return AcceleratorConfigResult::Success;
}

AcceleratorConfigResult AcceleratorConfiguration::doAddAccelerator(AcceleratorActionId id,
                                                                   const Accelerator &accelerator,
                                                                   bool saveOverride)
{
    const auto accelerators = m_idToAccelerators.find(id);
    if (accelerators == m_idToAccelerators.end())
        return AcceleratorConfigResult::NotFound;

    // Whoever holds the combination now loses it. A default that is taken
    // from its own action is not recorded as a removal: applying the add
    // override again reproduces it.
    if (const AcceleratorAction *conflict = findAcceleratorAction(accelerator)) {
        bool saveRemoveOverride = true;
        if (const auto defaultId = idForDefaultAccelerator(accelerator))
            saveRemoveOverride = *defaultId != *conflict;
        const AcceleratorConfigResult removed =
                doRemoveAccelerator(toActionId(*conflict), accelerator, saveRemoveOverride);
        if (removed != AcceleratorConfigResult::Success)
            return removed;
    }

    accelerators->second.push_back(accelerator);
    m_acceleratorToId.insertNew(accelerator, static_cast<AcceleratorAction>(id));

    if (saveOverride)
        updateOverrides(id, accelerator, ModificationAction::Add);
    return AcceleratorConfigResult::Success;
}

AcceleratorConfigResult
AcceleratorConfiguration::doReplaceAccelerator(AcceleratorActionId id,
                                               const Accelerator &oldAccelerator,
                                               const Accelerator &newAccelerator)
{
    const AcceleratorAction *foundId = m_acceleratorToId.find(oldAccelerator);
    if (!foundId || toActionId(*foundId) != id)
        return AcceleratorConfigResult::NotFound;

    const AcceleratorConfigResult removed =
            doRemoveAccelerator(id, oldAccelerator, /*saveOverride=*/true);
    if (removed != AcceleratorConfigResult::Success)
        return removed;
    return doAddAccelerator(id, newAccelerator, /*saveOverride=*/true);
}

void AcceleratorConfiguration::setUsePositionalLookup(bool use)
{
    m_acceleratorToId.setUsePositionalLookup(use);
    m_defaultAcceleratorsToId.setUsePositionalLookup(use);
}

void AcceleratorConfiguration::updateAndNotifyAccelerators()
{
    m_accelerators.clear();
    m_accelerators.reserve(m_acceleratorToId.size());
    for (const auto &[accelerator, action] : m_acceleratorToId)
        m_accelerators.push_back(accelerator);

    saveOverrides();
    Q_EMIT acceleratorsUpdated();
}

void AcceleratorConfiguration::loadOverrides()
{
    m_overrides.clear();
    m_settings.sync();
    m_settings.beginGroup(QLatin1String(kOverridesGroup));
    const QStringList actions = m_settings.childKeys();
    for (const QString &action : actions) {
        const QJsonDocument document =
                QJsonDocument::fromJson(m_settings.value(action).toString().toUtf8());
        if (document.isArray())
            m_overrides[action] = document.array();
    }
    m_settings.endGroup();
}

void AcceleratorConfiguration::saveOverrides()
{
    // The Command convention's shortcuts are fixed; the saved ones wait for PC.
    if (!customizable())
        return;
    m_settings.beginGroup(QLatin1String(kOverridesGroup));
    m_settings.remove(QString());
    for (const auto &[action, list] : m_overrides) {
        m_settings.setValue(action, QString::fromUtf8(QJsonDocument(list).toJson(
                                            QJsonDocument::Compact)));
    }
    m_settings.endGroup();
    m_settings.sync();
    if (m_settings.status() != QSettings::NoError)
        qCWarning(cfInput) << "Cannot persist shortcut overrides to" << m_settings.fileName();
}

void AcceleratorConfiguration::applyOverrides()
{
    std::vector<QString> actionsToRemove;

    for (auto &[actionName, list] : m_overrides) {
        const auto action = acceleratorActionFromName(actionName);
        if (!action || !isValid(toActionId(*action))) {
            qCInfo(cfInput) << "Dropping shortcut overrides for unknown action" << actionName;
            actionsToRemove.push_back(actionName);
            continue;
        }
        if (list.isEmpty()) {
            actionsToRemove.push_back(actionName);
            continue;
        }
        const AcceleratorActionId id = toActionId(*action);

        for (int i = 0; i < list.size();) {
            const auto modification = modificationFromJson(list.at(i));
            if (!modification) {
                qCWarning(cfInput) << "Malformed shortcut override for" << actionName;
                list.removeAt(i);
                continue;
            }
            if (modification->action == ModificationAction::Remove) {
                // A removal of something that no longer exists is stale; it
                // may also have been re-added to another action meanwhile.
                const AcceleratorAction *found =
                        m_acceleratorToId.find(modification->accelerator);
                if (!found) {
                    list.removeAt(i);
                    continue;
                }
                if (toActionId(*found) == id)
                    doRemoveAccelerator(id, modification->accelerator, /*saveOverride=*/false);
            } else {
                doAddAccelerator(id, modification->accelerator, /*saveOverride=*/false);
            }
            ++i;
        }
        if (list.isEmpty())
            actionsToRemove.push_back(actionName);
    }

    for (const QString &action : actionsToRemove)
        m_overrides.erase(action);

    if (!areAcceleratorsValid()) {
        qCWarning(cfInput) << "Shortcut overrides are inconsistent; restoring the defaults";
        restoreAllDefaults();
        return;
    }
    updateAndNotifyAccelerators();
}

void AcceleratorConfiguration::updateOverrides(AcceleratorActionId id,
                                               const Accelerator &accelerator,
                                               ModificationAction action)
{
    const QString key = acceleratorActionName(static_cast<AcceleratorAction>(id));
    auto entry = m_overrides.find(key);
    if (entry == m_overrides.end()) {
        m_overrides[key] = QJsonArray {modificationToJson(accelerator, action)};
        return;
    }

    QJsonArray &list = entry->second;
    for (int i = 0; i < list.size(); ++i) {
        const auto modification = modificationFromJson(list.at(i));
        if (modification && modification->accelerator == accelerator) {
            // The opposite modification of the same combination cancels it:
            // a custom accelerator removed again, or a default re-enabled.
            Q_ASSERT(modification->action != action);
            list.removeAt(i);
            if (list.isEmpty())
                m_overrides.erase(entry);
            return;
        }
    }
    list.append(modificationToJson(accelerator, action));
}

bool AcceleratorConfiguration::areAcceleratorsValid()
{
    for (const auto &[id, accelerators] : m_idToAccelerators) {
        for (const Accelerator &accelerator : accelerators) {
            const AcceleratorAction *found = m_acceleratorToId.find(accelerator);
            if (!found || toActionId(*found) != id)
                return false;
        }
    }
    for (const auto &[accelerator, action] : m_acceleratorToId) {
        const auto it = m_idToAccelerators.find(toActionId(action));
        if (it == m_idToAccelerators.end())
            return false;
        if (std::find(it->second.begin(), it->second.end(), accelerator) == it->second.end())
            return false;
    }
    return true;
}

void AcceleratorConfiguration::resetAllAccelerators()
{
    m_accelerators.clear();
    m_idToAccelerators = m_defaultIdToAccelerators;
    m_acceleratorToId = m_defaultAcceleratorsToId;
}

int AcceleratorConfiguration::totalNumberOfModifications() const
{
    int count = 0;
    for (const auto &[action, list] : m_overrides)
        count += list.size();
    return count;
}

} // namespace cutefish::accelerators
