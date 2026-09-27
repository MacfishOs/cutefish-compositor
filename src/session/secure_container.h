// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_SESSION_SECURE_CONTAINER_H_
#define CUTEFISH_SESSION_SECURE_CONTAINER_H_

#include <optional>
#include <vector>

#include <QFileSystemWatcher>
#include <QList>
#include <QRect>
#include <QString>

namespace cutefish::render {
class Renderer;
}

namespace cutefish::scene {
class ColorNode;
class Node;
} // namespace cutefish::scene

namespace cutefish::session {

struct SecureOutput {
    QString name;
    QRect bounds;
    // Device pixels per logical pixel, for content drawn at the output's density.
    double scale = 1;
    bool primary = false;

    friend bool operator==(const SecureOutput &, const SecureOutput &) = default;
};

// How far the lock screen has come in, as one frame draws it: the blurred
// wallpaper over the desktop, and the panel above it.
struct CoverSample {
    float backdrop = 1;
    float panelOpacity = 1;
    double panelScale = 1;
};

// The lock raises to the screen and lifts off it as ash's lock_state_controller.cc
// does, but in one step, and only the panel scales: the wallpaper would show its edges.
class SecureContainer
{
public:
    static constexpr int kCoverDurationMs = 360;
    static constexpr int kUncoverDurationMs = 300;

    explicit SecureContainer(scene::Node *root);
    SecureContainer(const SecureContainer &) = delete;
    SecureContainer &operator=(const SecureContainer &) = delete;
    ~SecureContainer();

    scene::Node *node() const { return m_root; }

    scene::Node *authContainer() const { return m_auth; }

    scene::Node *shieldContainer() const { return m_shield; }

    void setBlocked(bool blocked);

    bool isBlocked() const { return m_blocked; }

    // Brings the lock screen in (covered) or takes it away. Without `animate`
    // it is there at once, as at the login screen.
    void setCovered(bool covered, bool animate);

    // Ends a transition where it is headed, for a lock taken as the displays go
    // dark: they must wake to the lock screen, not to the rest of its fade.
    void finishTransition();

    // The transition as a frame drawn at `frameTime` shows it, in frame-clock
    // milliseconds. The first frame drawn starts it.
    CoverSample sample(double frameTime, double framePeriod) const;
    CoverSample sample(const render::Renderer &renderer) const;

    // Whether a frame drawn at `frameTime` shows the transition over. A lock is
    // not in place until such a frame is on screen.
    bool isSettled(double frameTime) const;

    double lastSampleTime() const { return m_lastSample; }

    // Asks every node the transition moves for another frame.
    void requestAnimationFrame() const;

    void setOutputs(const QList<SecureOutput> &outputs);

    const QList<SecureOutput> &outputs() const { return m_outputs; }

    // Every output has its shield, and every shield's wallpaper has finished
    // loading, whether or not it could be read.
    bool isWallpaperReady() const;

private:
    void rebuildShield();
    void refreshWallpaper();
    QFileSystemWatcher m_wallpaperWatcher;
    QString m_wallpaperKey;

    int transitionDuration() const;

    scene::Node *m_root = nullptr;
    scene::Node *m_shield = nullptr;
    scene::Node *m_auth = nullptr;
    std::vector<scene::ColorNode *> m_shieldRects;
    QList<SecureOutput> m_outputs;
    bool m_blocked = false;
    bool m_covered = true;
    bool m_animating = false;
    double m_requested = 0;
    mutable std::optional<double> m_start;
    mutable double m_lastSample = -1;
};

} // namespace cutefish::session

#endif // CUTEFISH_SESSION_SECURE_CONTAINER_H_
