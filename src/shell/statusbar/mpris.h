// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_SHELL_STATUSBAR_MPRIS_H_
#define CUTEFISH_SHELL_STATUSBAR_MPRIS_H_

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "base/dbus.h"
#include "base/signal.h"
#include "shell/common/pending_calls.h"
#include "shell/common/sender_filter.h"

namespace cutefish::shell {

// One media player on the session bus, org.mpris.MediaPlayer2.<name>: its
// MediaPlayer2 and MediaPlayer2.Player properties, read once and kept from
// PropertiesChanged, and the calls the control centre's media controls make.
class MprisPlayer
{
public:
    enum class PlaybackStatus {
        Stopped,
        Playing,
        Paused,
    };

    MprisPlayer(base::dbus::Connection *bus, std::string service);
    ~MprisPlayer();

    MprisPlayer(const MprisPlayer &) = delete;
    MprisPlayer &operator=(const MprisPlayer &) = delete;

    const std::string &service() const { return m_service; }
    // Both interfaces answered once.
    bool isReady() const { return m_rootReady && m_playerReady; }

    std::string identity() const;

    PlaybackStatus playbackStatus() const;
    bool canControl() const;
    bool canGoNext() const;
    bool canGoPrevious() const;
    bool canPlay() const;
    bool canPause() const;
    // xesam:title, xesam:artist joined by ", ", mpris:artUrl.
    std::string title() const;
    std::string artist() const;
    std::string artUrl() const;

    // Methods of a player that cannot be controlled are not sent.
    void play();
    void pause();
    void playPause();
    void next();
    void previous();

    // Any property changed.
    base::Signal<> changed;
    base::Signal<> playbackStatusChanged;

private:
    void load(const std::string &interface);
    void apply(const std::string &interface, const base::dbus::VariantMap &properties);
    bool playerFlag(const char *name, bool fallback = false) const;
    void callPlayer(const char *method);

    base::dbus::Connection *m_bus;
    std::string m_service;
    base::dbus::Proxy m_root;
    base::dbus::Proxy m_player;
    SenderFilter m_sender;
    std::vector<base::dbus::Match> m_matches;
    PendingCalls m_calls;
    base::dbus::VariantMap m_rootProperties;
    base::dbus::VariantMap m_playerProperties;
    base::dbus::VariantMap m_metadata;
    bool m_rootReady = false;
    bool m_playerReady = false;
};

// The media players on the bus and the one the control centre shows: the
// one that last started playing. A player that appears while the shown one
// is not playing is shown; when the shown one stops, the next that is still
// playing takes its place; when it goes, the next one on the list does.
class MprisManager
{
public:
    explicit MprisManager(base::dbus::Connection *bus);
    ~MprisManager();

    MprisManager(const MprisManager &) = delete;
    MprisManager &operator=(const MprisManager &) = delete;

    MprisPlayer *current() const { return m_current.get(); }
    std::string currentService() const;
    // The shown player first.
    std::vector<std::string> availableServices() const;

    base::Signal<> currentChanged;
    // Something of the shown player changed.
    base::Signal<> currentPlayerChanged;

private:
    void appeared(const std::string &service);
    void vanished(const std::string &service);
    void playbackStatusChanged(const std::string &service);
    std::shared_ptr<MprisPlayer> available(const std::string &service) const;
    void setCurrent(std::shared_ptr<MprisPlayer> player);
    void track(const std::shared_ptr<MprisPlayer> &player);

    base::dbus::Connection *m_bus;
    std::shared_ptr<MprisPlayer> m_current;
    std::vector<std::shared_ptr<MprisPlayer>> m_available;
    // Playing, but not shown: the first takes over when the shown one stops.
    std::deque<std::shared_ptr<MprisPlayer>> m_otherPlaying;
    std::vector<std::pair<MprisPlayer *, base::Subscription>> m_subscriptions;
    base::Subscription m_currentSubscription;
    base::dbus::Match m_nameOwnerChanged;
    PendingCalls m_calls;
};

} // namespace cutefish::shell

#endif // CUTEFISH_SHELL_STATUSBAR_MPRIS_H_
