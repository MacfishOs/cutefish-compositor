// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.chromium file.
//
// Upstream: chromium/src/ash/webui/shortcut_customization_ui/backend/
//           accelerator_configuration_provider.h
// Revision: 8abbac30b9cf2fe976422c58c000c94685648b65
//
// Adapted for Cutefish: the mojo provider the Shortcuts app talks to becomes
// a D-Bus service, com.cutefish.Shortcuts1, carrying JSON. The layout table
// (categories, descriptions, order) lives with the Settings UI, which owns the
// translations; this side deals in action names and keysyms only.

#ifndef CUTEFISH_ACCELERATORS_ACCELERATOR_SERVICE_H_
#define CUTEFISH_ACCELERATORS_ACCELERATOR_SERVICE_H_

#include <optional>

#include <QJsonObject>
#include <QObject>
#include <QString>

#include "accelerators/accelerator.h"
#include "accelerators/accelerator_actions.h"
#include "accelerators/accelerator_configuration.h"

namespace cutefish::input {
class XkbTracker;
}

namespace cutefish::accelerators {

class AcceleratorController;

class AcceleratorService : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.cutefish.Shortcuts1")
    // "pc", or "command" when Meta is the Command key next to the space bar,
    // Alt is Option and Ctrl is Control; see input::ShortcutConvention.
    Q_PROPERTY(QString shortcutConvention READ shortcutConvention NOTIFY acceleratorsChanged)
    // False under the Command convention, whose shortcuts are fixed: every
    // change is answered with action-locked.
    Q_PROPERTY(bool customizable READ customizable NOTIFY acceleratorsChanged)

public:
    AcceleratorService(AcceleratorConfiguration *configuration, AcceleratorController *controller,
                       const input::XkbTracker *xkb, QObject *parent = nullptr);
    ~AcceleratorService() override;

    bool registerService();

    QString shortcutConvention() const;
    bool customizable() const;

    // JSON for one accelerator, shared with the tests.
    QJsonObject acceleratorToJson(const Accelerator &accelerator, AcceleratorActionId id) const;

public Q_SLOTS:
    // {"actions": [{"name", "accelerators": [...], "defaults": [...]}]}.
    QString accelerators();

    // Every mutation answers {"result": <AcceleratorConfigResult name>,
    // "conflictAction": <action name, when the result names one>}. `keycode`
    // is the evdev code of the key the user pressed, 0 when unknown, and
    // `keysym` the keysym the toolkit reported for it. A Meta keycode with no
    // modifiers binds the Meta tap.
    QString addAccelerator(const QString &action, uint keycode, uint keysym, uint modifiers);
    QString removeAccelerator(const QString &action, uint keysym, uint modifiers);
    QString replaceAccelerator(const QString &action, uint oldKeysym, uint oldModifiers,
                               uint keycode, uint keysym, uint modifiers);
    QString restoreDefault(const QString &action);
    QString restoreAllDefaults();

    // {"keysym", "keyName", "keyText", "modifierKey"} for a key the recorder
    // saw. A lone Meta (no other modifier) is accepted as a shortcut that
    // triggers on release; other modifiers on their own are not.
    QString describeKey(uint keycode, uint keysym);

    // True while the user is recording a shortcut, so that the keys they
    // press do not also trigger anything. Also resets the confirmation
    // state of a pending conflict or warning.
    void preventProcessingAccelerators(bool prevent);

    bool hasCustomAccelerators();

Q_SIGNALS:
    void acceleratorsChanged();

private:
    // Which confirmation the last answer asked the user for.
    enum class ConflictErrorState {
        Standby,
        AwaitingConflictResolution,
        AwaitingNonMetaConfirmation,
    };

    struct PendingAccelerator {
        Accelerator accelerator;
        AcceleratorActionId action;
    };

    Accelerator resolve(uint keycode, uint keysym, uint modifiers) const;
    std::optional<AcceleratorConfigResult> validateAction(const QString &name,
                                                          AcceleratorActionId *id) const;
    std::optional<AcceleratorConfigResult> validateAccelerator(const Accelerator &accelerator) const;
    std::optional<QJsonObject> preprocessAddAccelerator(AcceleratorActionId id,
                                                        const Accelerator &accelerator);
    ConflictErrorState maybeHandleNonMetaAccelerator(const Accelerator &accelerator,
                                                     AcceleratorActionId id);
    QString result(AcceleratorConfigResult result, const QString &conflictAction = {}) const;

    AcceleratorConfiguration *m_configuration = nullptr;
    AcceleratorController *m_controller = nullptr;
    const input::XkbTracker *m_xkb = nullptr;
    bool m_registered = false;

    std::optional<PendingAccelerator> m_pending;
    ConflictErrorState m_conflictState = ConflictErrorState::Standby;
};

} // namespace cutefish::accelerators

#endif // CUTEFISH_ACCELERATORS_ACCELERATOR_SERVICE_H_
