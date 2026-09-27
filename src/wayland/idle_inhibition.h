// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <QObject>

namespace cutefish::input {
class InputManager;
}

namespace cutefish::wayland {

// Shared inhibition count. Wayland inhibitors and the org.freedesktop.ScreenSaver
// service (session::ScreenSaverService) acquire it; idle notifications read it.
class IdleInhibition : public QObject
{
    Q_OBJECT
public:
    explicit IdleInhibition(QObject *parent = nullptr);
    ~IdleInhibition() override;

    bool inhibited() const { return m_count != 0 && !m_ignored; }

    void acquire();
    void release();

    // Nothing a client shows can be seen on the lock screen, so its inhibitors
    // wait until the session is unblocked.
    void setIgnored(bool ignored);
    input::InputManager *input = nullptr;

Q_SIGNALS:
    void inhibitedChanged();
    void sceneChanged();

private:
    size_t m_count = 0;
    bool m_ignored = false;
};
} // namespace cutefish::wayland
