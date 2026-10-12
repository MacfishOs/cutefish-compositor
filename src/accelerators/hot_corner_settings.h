// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <array>
#include <functional>
#include <optional>

#include <QObject>
#include <QSettings>

#include "accelerators/accelerator_actions.h"

namespace cutefish::accelerators {

enum class HotCorner {
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

// What a corner runs, and the modifiers that must be held for it; none means
// any may be.
struct HotCornerBinding {
    std::optional<AcceleratorAction> action;
    uint32_t modifiers = 0;

    bool operator==(const HotCornerBinding &) const = default;
};

// The actions of the four screen corners, kept in compositor.conf [HotCorners]
// and exported as com.cutefish.HotCorners1. An action is named as in the
// shortcuts service; an empty one turns the corner off.
class HotCornerSettings : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.cutefish.HotCorners1")
    Q_PROPERTY(QString topLeft READ topLeft NOTIFY changed)
    Q_PROPERTY(int topLeftModifiers READ topLeftModifiers NOTIFY changed)
    Q_PROPERTY(QString topRight READ topRight NOTIFY changed)
    Q_PROPERTY(int topRightModifiers READ topRightModifiers NOTIFY changed)
    Q_PROPERTY(QString bottomLeft READ bottomLeft NOTIFY changed)
    Q_PROPERTY(int bottomLeftModifiers READ bottomLeftModifiers NOTIFY changed)
    Q_PROPERTY(QString bottomRight READ bottomRight NOTIFY changed)
    Q_PROPERTY(int bottomRightModifiers READ bottomRightModifiers NOTIFY changed)

public:
    explicit HotCornerSettings(QObject *parent = nullptr, const QString &settingsFile = {});
    ~HotCornerSettings() override;

    bool registerService();

    const HotCornerBinding &binding(HotCorner corner) const;

    // What heldModifiers() answers from: the seat the corners read.
    void setModifierSource(std::function<uint32_t()> source) { m_modifierSource = std::move(source); }

    QString topLeft() const { return actionName(HotCorner::TopLeft); }
    int topLeftModifiers() const { return modifiers(HotCorner::TopLeft); }
    QString topRight() const { return actionName(HotCorner::TopRight); }
    int topRightModifiers() const { return modifiers(HotCorner::TopRight); }
    QString bottomLeft() const { return actionName(HotCorner::BottomLeft); }
    int bottomLeftModifiers() const { return modifiers(HotCorner::BottomLeft); }
    QString bottomRight() const { return actionName(HotCorner::BottomRight); }
    int bottomRightModifiers() const { return modifiers(HotCorner::BottomRight); }

public slots:
    // `corner` is top-left, top-right, bottom-left or bottom-right. An action
    // that is not known turns the corner off.
    void setCorner(const QString &corner, const QString &action, int modifiers);

    // The modifiers held right now, as a corner sees them, for a settings page
    // that takes a corner's modifiers from the keys held while choosing. A
    // client's own modifiers can differ: the Command convention swaps keys
    // before applications see them.
    int heldModifiers() const;

signals:
    void changed();

private:
    QString actionName(HotCorner corner) const;
    int modifiers(HotCorner corner) const;

    QSettings m_settings;
    std::array<HotCornerBinding, 4> m_bindings;
    std::function<uint32_t()> m_modifierSource;
    bool m_registered = false;
};

} // namespace cutefish::accelerators
