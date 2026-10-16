// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/statusbar/mpris.h"

#include <algorithm>

#include "base/strings.h"

namespace cutefish::shell {

namespace dbus = base::dbus;

static constexpr const char *kPrefix = "org.mpris.MediaPlayer2.";
static constexpr const char *kPath = "/org/mpris/MediaPlayer2";
static constexpr const char *kRootInterface = "org.mpris.MediaPlayer2";
static constexpr const char *kPlayerInterface = "org.mpris.MediaPlayer2.Player";

MprisPlayer::MprisPlayer(dbus::Connection *bus, std::string service)
    : m_bus(bus)
    , m_service(std::move(service))
    , m_root(bus, m_service, kPath, kRootInterface)
    , m_player(bus, m_service, kPath, kPlayerInterface)
    , m_sender(bus, m_service)
{
    if (m_bus) {
        dbus::MatchRule rule;
        rule.sender = m_service;
        rule.path = kPath;
        rule.interface = "org.freedesktop.DBus.Properties";
        rule.member = "PropertiesChanged";
        m_matches.push_back(m_bus->subscribe(rule, [this](const dbus::Message &message) {
            std::string interface;
            dbus::VariantMap changed;
            std::vector<std::string> invalidated;
            if (!m_sender.accepts(message) || !message.read(interface, changed, invalidated))
                return;
            if (interface != kRootInterface && interface != kPlayerInterface)
                return;
            apply(interface, changed);
            if (!invalidated.empty())
                load(interface);
        }));
    }
    load(kRootInterface);
    load(kPlayerInterface);
}

MprisPlayer::~MprisPlayer() = default;

void MprisPlayer::load(const std::string &interface)
{
    const dbus::Proxy &proxy = interface == kRootInterface ? m_root : m_player;
    m_calls.track([&](std::function<void()> finished) {
        return proxy.getAllAsync([this, interface, finished](const dbus::Reply &reply) {
            if (interface == kRootInterface)
                m_rootReady = true;
            else
                m_playerReady = true;
            apply(interface, reply.value<dbus::VariantMap>().value_or(dbus::VariantMap()));
            finished();
        });
    });
}

void MprisPlayer::apply(const std::string &interface, const dbus::VariantMap &properties)
{
    const bool root = interface == kRootInterface;
    dbus::VariantMap &stored = root ? m_rootProperties : m_playerProperties;
    const PlaybackStatus status = playbackStatus();
    for (const auto &[name, value] : properties) {
        stored[name] = value;
        if (!root && name == "Metadata")
            m_metadata = value.value<dbus::VariantMap>();
    }
    changed();
    if (playbackStatus() != status)
        playbackStatusChanged();
}

static std::string stringOf(const dbus::VariantMap &properties, const char *name)
{
    const auto it = properties.find(name);
    return it == properties.end() ? std::string() : it->second.value<std::string>();
}

static bool flagOf(const dbus::VariantMap &properties, const char *name, bool fallback)
{
    const auto it = properties.find(name);
    return it == properties.end() ? fallback : it->second.value<bool>(fallback);
}

std::string MprisPlayer::identity() const
{
    return stringOf(m_rootProperties, "Identity");
}

MprisPlayer::PlaybackStatus MprisPlayer::playbackStatus() const
{
    const std::string status = stringOf(m_playerProperties, "PlaybackStatus");
    if (status == "Playing")
        return PlaybackStatus::Playing;
    if (status == "Paused")
        return PlaybackStatus::Paused;
    return PlaybackStatus::Stopped;
}

bool MprisPlayer::playerFlag(const char *name, bool fallback) const
{
    return flagOf(m_playerProperties, name, fallback);
}

bool MprisPlayer::canControl() const
{
    return playerFlag("CanControl");
}

bool MprisPlayer::canGoNext() const
{
    return playerFlag("CanGoNext");
}

bool MprisPlayer::canGoPrevious() const
{
    return playerFlag("CanGoPrevious");
}

bool MprisPlayer::canPlay() const
{
    return playerFlag("CanPlay");
}

bool MprisPlayer::canPause() const
{
    return playerFlag("CanPause");
}

std::string MprisPlayer::title() const
{
    return stringOf(m_metadata, "xesam:title");
}

std::string MprisPlayer::artist() const
{
    const auto it = m_metadata.find("xesam:artist");
    if (it == m_metadata.end())
        return {};
    // A list by the spec; some players send one string.
    if (auto artists = it->second.get<std::vector<std::string>>())
        return base::strings::join(*artists, ", ");
    return it->second.value<std::string>();
}

std::string MprisPlayer::artUrl() const
{
    return stringOf(m_metadata, "mpris:artUrl");
}

void MprisPlayer::callPlayer(const char *method)
{
    if (canControl())
        m_player.send(method);
}

void MprisPlayer::play()
{
    callPlayer("Play");
}

void MprisPlayer::pause()
{
    callPlayer("Pause");
}

void MprisPlayer::playPause()
{
    callPlayer("PlayPause");
}

void MprisPlayer::next()
{
    callPlayer("Next");
}

void MprisPlayer::previous()
{
    callPlayer("Previous");
}

MprisManager::MprisManager(dbus::Connection *bus)
    : m_bus(bus)
{
    if (!m_bus)
        return;

    dbus::MatchRule rule;
    rule.sender = "org.freedesktop.DBus";
    rule.path = "/org/freedesktop/DBus";
    rule.interface = "org.freedesktop.DBus";
    rule.member = "NameOwnerChanged";
    m_nameOwnerChanged = m_bus->subscribe(rule, [this](const dbus::Message &message) {
        std::string name, oldOwner, newOwner;
        if (!message.read(name, oldOwner, newOwner) || !name.starts_with(kPrefix))
            return;
        if (oldOwner.empty())
            appeared(name);
        else if (newOwner.empty())
            vanished(name);
    });

    const dbus::Proxy daemon(m_bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus");
    m_calls.track([&](std::function<void()> finished) {
        return daemon.callAsync("ListNames", [this, finished](const dbus::Reply &reply) {
            for (const std::string &name : reply.value<std::vector<std::string>>().value_or(std::vector<std::string>())) {
                if (name.starts_with(kPrefix))
                    appeared(name);
            }
            finished();
        });
    });
}

MprisManager::~MprisManager() = default;

std::string MprisManager::currentService() const
{
    return m_current ? m_current->service() : std::string();
}

std::vector<std::string> MprisManager::availableServices() const
{
    std::vector<std::string> services;
    for (const auto &player : m_available)
        services.push_back(player->service());
    return services;
}

std::shared_ptr<MprisPlayer> MprisManager::available(const std::string &service) const
{
    for (const auto &player : m_available) {
        if (player->service() == service)
            return player;
    }
    return nullptr;
}

void MprisManager::track(const std::shared_ptr<MprisPlayer> &player)
{
    MprisPlayer *raw = player.get();
    for (const auto &[tracked, subscription] : m_subscriptions) {
        if (tracked == raw)
            return;
    }
    const std::string service = player->service();
    m_subscriptions.emplace_back(raw, player->playbackStatusChanged.connect([this, service] {
        playbackStatusChanged(service);
    }));
}

void MprisManager::setCurrent(std::shared_ptr<MprisPlayer> player)
{
    if (player == m_current)
        return;
    m_current = std::move(player);
    m_currentSubscription = m_current ? m_current->changed.connect([this] { currentPlayerChanged(); })
                                      : base::Subscription();
    currentChanged();
}

void MprisManager::appeared(const std::string &service)
{
    std::shared_ptr<MprisPlayer> player = available(service);
    if (player) {
        std::erase(m_available, player);
        std::erase(m_otherPlaying, player);
    } else {
        player = m_current && m_current->service() == service ? m_current
                                                               : std::make_shared<MprisPlayer>(m_bus, service);
        track(player);
    }

    if (!m_current)
        setCurrent(player);

    if (player == m_current) {
        m_available.insert(m_available.begin(), player);
        return;
    }

    if (m_current->playbackStatus() != MprisPlayer::PlaybackStatus::Playing) {
        m_available.insert(m_available.begin(), player);
        setCurrent(player);
    } else {
        m_available.insert(m_available.begin() + std::min<ptrdiff_t>(1, ptrdiff_t(m_available.size())), player);
        if (player->playbackStatus() == MprisPlayer::PlaybackStatus::Playing)
            m_otherPlaying.push_front(player);
    }
}

void MprisManager::vanished(const std::string &service)
{
    if (std::shared_ptr<MprisPlayer> player = available(service)) {
        std::erase(m_available, player);
        std::erase(m_otherPlaying, player);
        std::erase_if(m_subscriptions, [&player](const auto &entry) { return entry.first == player.get(); });
    }

    if (m_current && m_current->service() == service)
        setCurrent(m_available.empty() ? nullptr : m_available.front());
}

void MprisManager::playbackStatusChanged(const std::string &service)
{
    std::shared_ptr<MprisPlayer> player = available(service);
    if (!player)
        return;

    if (player == m_current) {
        if (m_current->playbackStatus() == MprisPlayer::PlaybackStatus::Playing || m_otherPlaying.empty())
            return;
        std::shared_ptr<MprisPlayer> next = m_otherPlaying.front();
        m_otherPlaying.pop_front();
        if (const auto it = std::find(m_available.begin(), m_available.end(), next); it != m_available.end()) {
            m_available.erase(it);
            m_available.insert(m_available.begin(), next);
        }
        setCurrent(next);
        return;
    }

    if (player->playbackStatus() != MprisPlayer::PlaybackStatus::Playing) {
        std::erase(m_otherPlaying, player);
        return;
    }

    if (!m_current || m_current->playbackStatus() != MprisPlayer::PlaybackStatus::Playing) {
        setCurrent(player);
    } else {
        std::erase(m_available, player);
        m_available.insert(m_available.begin() + std::min<ptrdiff_t>(1, ptrdiff_t(m_available.size())), player);
        std::erase(m_otherPlaying, player);
        m_otherPlaying.push_front(player);
    }
}

} // namespace cutefish::shell
