// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ash/webui/shortcut_customization_ui/backend/
//           accelerator_configuration_provider.cc
// Revision: 8abbac30b9cf2fe976422c58c000c94685648b65
//
// Adapted for Cutefish: integrates the upstream behavior with native
// compositor types and ownership.

#include "accelerators/accelerator_service.h"

#include <algorithm>

#include <QDBusConnection>
#include <QDBusError>
#include <QJsonArray>
#include <QJsonDocument>
#include <xkbcommon/xkbcommon.h>

#include "accelerators/accelerator_controller.h"
#include "core/logging.h"
#include "input/xkb_tracker.h"

namespace cutefish::accelerators {

namespace {

constexpr auto kService = "com.cutefish.Shortcuts1";
constexpr auto kPath = "/com/cutefish/Shortcuts1";

constexpr size_t kMaxAcceleratorsAllowed = 5;

// Only these modifiers take part in a customized accelerator.
constexpr uint32_t kCustomizationModifierMask = ShiftDown | ControlDown | AltDown | MetaDown;

// Keys that may not be the activation key of a custom accelerator: power
// control, and the locks whose state xkb owns.
constexpr uint32_t kReservedKeys[] = {
        XKB_KEY_XF86PowerOff, XKB_KEY_XF86Sleep, XKB_KEY_Caps_Lock,
        XKB_KEY_Scroll_Lock,  XKB_KEY_Num_Lock,
};

bool isMetaKeysym(uint32_t keysym)
{
    return keysym == XKB_KEY_Super_L || keysym == XKB_KEY_Super_R;
}

QString toJsonString(const QJsonObject &object)
{
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

} // namespace

AcceleratorService::AcceleratorService(AcceleratorConfiguration *configuration,
                                       AcceleratorController *controller,
                                       const input::XkbTracker *xkb, QObject *parent)
    : QObject(parent)
    , m_configuration(configuration)
    , m_controller(controller)
    , m_xkb(xkb)
{
    connect(m_configuration, &AcceleratorConfiguration::acceleratorsUpdated, this,
            &AcceleratorService::acceleratorsChanged);
}

AcceleratorService::~AcceleratorService()
{
    if (m_registered) {
        auto bus = QDBusConnection::sessionBus();
        bus.unregisterObject(QLatin1String(kPath));
        bus.unregisterService(QLatin1String(kService));
    }
}

QString AcceleratorService::shortcutConvention() const
{
    return m_configuration->convention() == input::ShortcutConvention::Command
            ? QStringLiteral("command")
            : QStringLiteral("pc");
}

bool AcceleratorService::registerService()
{
    if (m_registered)
        return true;
    auto bus = QDBusConnection::sessionBus();
    if (!bus.registerObject(QLatin1String(kPath), this,
                            QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals
                                    | QDBusConnection::ExportAllProperties)) {
        qCWarning(cfInput) << "Cannot export the shortcuts service:" << bus.lastError().message();
        return false;
    }
    if (!bus.registerService(QLatin1String(kService))) {
        qCWarning(cfInput) << "Cannot register the shortcuts service:"
                           << bus.lastError().message();
        bus.unregisterObject(QLatin1String(kPath));
        return false;
    }
    m_registered = true;
    return true;
}

QJsonObject AcceleratorService::acceleratorToJson(const Accelerator &accelerator,
                                                  AcceleratorActionId id) const
{
    QJsonObject object;
    object.insert(QStringLiteral("keysym"), static_cast<qint64>(accelerator.keysym()));
    object.insert(QStringLiteral("keyName"), keysymName(accelerator.keysym()));
    object.insert(QStringLiteral("keyText"), keysymText(accelerator.keysym()));
    object.insert(QStringLiteral("modifiers"), static_cast<qint64>(accelerator.modifiers()));
    object.insert(QStringLiteral("keyState"),
                  accelerator.keyState() == Accelerator::KeyState::Pressed
                          ? QStringLiteral("pressed")
                          : QStringLiteral("released"));
    const auto defaultId = m_configuration->idForDefaultAccelerator(accelerator);
    object.insert(QStringLiteral("type"),
                  defaultId && toActionId(*defaultId) == id ? QStringLiteral("default")
                                                            : QStringLiteral("user"));
    object.insert(QStringLiteral("locked"), m_configuration->isAcceleratorLocked(accelerator));
    return object;
}

QString AcceleratorService::accelerators()
{
    QJsonArray actions;
    for (const auto &[id, accelerators] : m_configuration->acceleratorMapping()) {
        QJsonObject entry;
        entry.insert(QStringLiteral("name"),
                     acceleratorActionName(static_cast<AcceleratorAction>(id)));
        QJsonArray current;
        for (const Accelerator &accelerator : accelerators)
            current.append(acceleratorToJson(accelerator, id));
        entry.insert(QStringLiteral("accelerators"), current);
        QJsonArray defaults;
        for (const Accelerator &accelerator : m_configuration->defaultAcceleratorsForId(id))
            defaults.append(acceleratorToJson(accelerator, id));
        entry.insert(QStringLiteral("defaults"), defaults);
        actions.append(entry);
    }
    QJsonObject root;
    root.insert(QStringLiteral("actions"), actions);
    return toJsonString(root);
}

Accelerator AcceleratorService::resolve(uint keycode, uint keysym, uint modifiers) const
{
    // The compositor's keymap is what the client typed against, so the evdev
    // code gives the same base keysym the filter will see.
    uint32_t resolved = keycode != 0 && m_xkb ? m_xkb->baseKeysym(keycode) : 0;
    if (resolved == 0)
        resolved = xkb_keysym_to_lower(keysym);
    // On a positional layout a punctuation key is stored as the US key at its
    // position, which is what the filter will look it up by.
    if (m_configuration->usesPositionalLookup() && keycode != 0) {
        if (const uint32_t positional = positionalUsKeysym(keycode, resolved))
            resolved = positional;
    }
    // A lone Meta is Ash's launcher key: it triggers on release, so that
    // Meta+<key> can still be typed with Meta held, and the launcher state
    // machine decides whether the tap was really alone.
    const uint32_t customizationModifiers = modifiers & kCustomizationModifierMask;
    const bool metaTap = isMetaKeysym(resolved) && customizationModifiers == NoModifier;
    return Accelerator(resolved, keycode, customizationModifiers,
                       metaTap ? Accelerator::KeyState::Released : Accelerator::KeyState::Pressed);
}

std::optional<AcceleratorConfigResult>
AcceleratorService::validateAction(const QString &name, AcceleratorActionId *id) const
{
    const auto action = acceleratorActionFromName(name);
    if (!action || !m_configuration->isValid(toActionId(*action)))
        return AcceleratorConfigResult::NotFound;
    *id = toActionId(*action);
    return std::nullopt;
}

std::optional<AcceleratorConfigResult>
AcceleratorService::validateAccelerator(const Accelerator &accelerator) const
{
    const uint32_t modifiers = accelerator.modifiers() & kCustomizationModifierMask;

    if (accelerator.keyState() == Accelerator::KeyState::Released)
        return std::nullopt;

    // Modifier + key, unless the key is a function key.
    if (modifiers == NoModifier && !isFunctionKeysym(accelerator.keysym()))
        return AcceleratorConfigResult::MissingModifier;

    if (std::find(std::begin(kReservedKeys), std::end(kReservedKeys), accelerator.keysym())
        != std::end(kReservedKeys)) {
        return AcceleratorConfigResult::ReservedKeyNotAllowed;
    }

    // A modifier cannot be the activation key of a custom accelerator.
    if (isModifierKeysym(accelerator.keysym()) || accelerator.keysym() == 0)
        return AcceleratorConfigResult::KeyNotAllowed;

    if (modifiers == ShiftDown)
        return AcceleratorConfigResult::ShiftOnlyNotAllowed;

    return std::nullopt;
}

std::optional<QJsonObject>
AcceleratorService::preprocessAddAccelerator(AcceleratorActionId id, const Accelerator &accelerator)
{
    const AcceleratorAction *found = m_configuration->findAcceleratorAction(accelerator);
    if (!found)
        return std::nullopt;

    const QString conflictName = acceleratorActionName(*found);

    // Already one of this action's own accelerators.
    if (toActionId(*found) == id) {
        m_pending.reset();
        QJsonObject object;
        object.insert(QStringLiteral("result"),
                      QString::fromLatin1(toString(AcceleratorConfigResult::Conflict)));
        object.insert(QStringLiteral("conflictAction"), conflictName);
        return object;
    }

    // Taking a combination from another action needs a second, identical
    // request as confirmation.
    if (!m_pending || m_pending->action != id || m_pending->accelerator != accelerator) {
        m_pending = PendingAccelerator {accelerator, id};
        m_conflictState = ConflictErrorState::AwaitingConflictResolution;
        QJsonObject object;
        object.insert(QStringLiteral("result"),
                      QString::fromLatin1(toString(AcceleratorConfigResult::ConflictCanOverride)));
        object.insert(QStringLiteral("conflictAction"), conflictName);
        return object;
    }
    if (m_conflictState == ConflictErrorState::AwaitingConflictResolution)
        m_conflictState = ConflictErrorState::Standby;
    return std::nullopt;
}

AcceleratorService::ConflictErrorState
AcceleratorService::maybeHandleNonMetaAccelerator(const Accelerator &accelerator,
                                                  AcceleratorActionId id)
{
    // Re-adding a default is never warned about.
    const std::vector<Accelerator> defaults = m_configuration->defaultAcceleratorsForId(id);
    if (std::find(defaults.begin(), defaults.end(), accelerator) != defaults.end())
        return ConflictErrorState::Standby;

    if (m_conflictState != ConflictErrorState::AwaitingNonMetaConfirmation)
        m_pending.reset();

    // Applications never see Meta. Under the Command convention they get both
    // Command and Control as Ctrl, so only the two together are clear of them.
    const bool clear = m_configuration->convention() == input::ShortcutConvention::Command
            ? accelerator.isMetaDown() && accelerator.isControlDown()
            : accelerator.isMetaDown();
    // Function keys cannot be used with Meta, so they are not warned about;
    // neither is the Meta tap itself.
    if (!clear && !isFunctionKeysym(accelerator.keysym())
        && accelerator.keyState() == Accelerator::KeyState::Pressed) {
        if (!m_pending || m_pending->action != id || m_pending->accelerator != accelerator) {
            m_pending = PendingAccelerator {accelerator, id};
            return ConflictErrorState::AwaitingNonMetaConfirmation;
        }
    }
    return ConflictErrorState::Standby;
}

QString AcceleratorService::result(AcceleratorConfigResult result,
                                   const QString &conflictAction) const
{
    QJsonObject object;
    object.insert(QStringLiteral("result"), QString::fromLatin1(toString(result)));
    if (!conflictAction.isEmpty())
        object.insert(QStringLiteral("conflictAction"), conflictAction);
    return toJsonString(object);
}

QString AcceleratorService::addAccelerator(const QString &action, uint keycode, uint keysym,
                                           uint modifiers)
{
    AcceleratorActionId id = 0;
    if (const auto error = validateAction(action, &id))
        return result(*error);
    const Accelerator accelerator = resolve(keycode, keysym, modifiers);

    const std::vector<Accelerator> defaults = m_configuration->defaultAcceleratorsForId(id);
    if (std::find(defaults.begin(), defaults.end(), accelerator) == defaults.end()) {
        if (const auto error = validateAccelerator(accelerator)) {
            m_pending.reset();
            return result(*error);
        }
    }

    if (const auto current = m_configuration->acceleratorsForAction(id);
        current && current->size() >= kMaxAcceleratorsAllowed) {
        return result(AcceleratorConfigResult::MaximumAcceleratorsReached);
    }

    if (const auto conflict = preprocessAddAccelerator(id, accelerator))
        return toJsonString(*conflict);

    m_conflictState = maybeHandleNonMetaAccelerator(accelerator, id);
    if (m_conflictState != ConflictErrorState::Standby)
        return result(AcceleratorConfigResult::NonMetaAcceleratorWarning);

    m_pending.reset();
    return result(m_configuration->addUserAccelerator(id, accelerator));
}

QString AcceleratorService::removeAccelerator(const QString &action, uint keysym, uint modifiers)
{
    AcceleratorActionId id = 0;
    if (const auto error = validateAction(action, &id))
        return result(*error);
    Accelerator accelerator = resolve(0, keysym, modifiers);
    // A default that triggers on release is one the caller does not know
    // about: it only ever sees presses.
    if (!m_configuration->findAcceleratorAction(accelerator)) {
        accelerator.setKeyState(Accelerator::KeyState::Released);
        if (!m_configuration->findAcceleratorAction(accelerator))
            accelerator.setKeyState(Accelerator::KeyState::Pressed);
    }
    return result(m_configuration->removeAccelerator(id, accelerator));
}

QString AcceleratorService::replaceAccelerator(const QString &action, uint oldKeysym,
                                               uint oldModifiers, uint keycode, uint keysym,
                                               uint modifiers)
{
    AcceleratorActionId id = 0;
    if (const auto error = validateAction(action, &id))
        return result(*error);
    Accelerator oldAccelerator = resolve(0, oldKeysym, oldModifiers);
    if (!m_configuration->findAcceleratorAction(oldAccelerator)) {
        oldAccelerator.setKeyState(Accelerator::KeyState::Released);
        if (!m_configuration->findAcceleratorAction(oldAccelerator))
            oldAccelerator.setKeyState(Accelerator::KeyState::Pressed);
    }
    const Accelerator newAccelerator = resolve(keycode, keysym, modifiers);

    const std::vector<Accelerator> defaults = m_configuration->defaultAcceleratorsForId(id);
    if (std::find(defaults.begin(), defaults.end(), newAccelerator) == defaults.end()) {
        if (const auto error = validateAccelerator(newAccelerator))
            return result(*error);
    }

    const AcceleratorAction *oldId = m_configuration->findAcceleratorAction(oldAccelerator);
    if (!oldId || toActionId(*oldId) != id)
        return result(AcceleratorConfigResult::NotFound);

    if (const auto conflict = preprocessAddAccelerator(id, newAccelerator))
        return toJsonString(*conflict);

    m_conflictState = maybeHandleNonMetaAccelerator(newAccelerator, id);
    if (m_conflictState != ConflictErrorState::Standby)
        return result(AcceleratorConfigResult::NonMetaAcceleratorWarning);

    m_pending.reset();
    return result(m_configuration->replaceAccelerator(id, oldAccelerator, newAccelerator));
}

QString AcceleratorService::restoreDefault(const QString &action)
{
    AcceleratorActionId id = 0;
    if (const auto error = validateAction(action, &id))
        return result(*error);
    return result(m_configuration->restoreDefault(id));
}

QString AcceleratorService::restoreAllDefaults()
{
    return result(m_configuration->restoreAllDefaults());
}

QString AcceleratorService::describeKey(uint keycode, uint keysym)
{
    const Accelerator accelerator = resolve(keycode, keysym, NoModifier);
    QJsonObject object;
    object.insert(QStringLiteral("keysym"), static_cast<qint64>(accelerator.keysym()));
    object.insert(QStringLiteral("keyName"), keysymName(accelerator.keysym()));
    object.insert(QStringLiteral("keyText"), keysymText(accelerator.keysym()));
    object.insert(QStringLiteral("modifierKey"), isModifierKeysym(accelerator.keysym()));
    return toJsonString(object);
}

void AcceleratorService::preventProcessingAccelerators(bool prevent)
{
    // Ash resets the pending state whenever the recorder starts or stops.
    m_pending.reset();
    m_conflictState = ConflictErrorState::Standby;
    m_controller->setPreventProcessingAccelerators(prevent);
}

bool AcceleratorService::hasCustomAccelerators()
{
    return m_configuration->hasCustomAccelerators();
}

} // namespace cutefish::accelerators
