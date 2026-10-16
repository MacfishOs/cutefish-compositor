// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/controlcenter/session_services.h"

#include <algorithm>
#include <filesystem>
#include <vector>

#include "audio/audiomodel.h"
#include "audio/backend.h"
#include "base/dbus.h"
#include "base/logging.h"
#include "base/thread_pool.h"
#include "base/timer.h"
#include "bluez/manager.h"
#include "networkmanager/client.h"
#include "networkmanager/handler.h"
#include "networkmanager/hotspot.h"
#include "networkmanager/networkmodel.h"
#include "networkmanager/wifiutils.h"
#include "powerprofiles/powerprofiles.h"
#include "shell/common/app_launcher.h"
#include "shell/common/pending_calls.h"
#include "shell/notification/notification_settings.h"
#include "shell/statusbar/status_bar.h"
#include "upower/batterystatus.h"

namespace cutefish::shell {

namespace dbus = base::dbus;
namespace nm = networkmanager;

static const base::LogCategory cfControlCenter("cutefish.shell.controlcenter");

// com.cutefish.Privacy: the camera and microphone switches.
static const char kPrivacyService[] = "com.cutefish.Privacy";
static const char kPrivacyPath[] = "/com/cutefish/Privacy";
static const char kPrivacyInterface[] = "com.cutefish.Privacy";
// The service's state: read here so that showing the switches starts no root process.
static const char kPrivacyState[] = "/var/lib/cutefish-privacy/";

// Settings' join window finds only access points NetworkManager lists; a
// remembered one waits for the next scan to list its network again.
struct PendingJoin {
    std::string accessPoint;
    std::string device;
    dbus::VariantMap properties;
};

class SessionServices final : public ControlCenterServices
{
public:
    SessionServices(dbus::Connection *sessionBus, StatusBar &statusBar, NotificationSettings *notifications)
        : m_statusBar(statusBar)
        , m_notifications(notifications)
        , m_client(nm::Client::instance())
        , m_handler(std::make_unique<nm::Handler>())
        , m_networks(std::make_unique<nm::NetworkModel>())
        , m_bluetooth(bluez::Manager::shared())
        , m_audio(std::make_unique<audio::AudioModel>(audio::AudioObject::Sink, audio::Backend::shared()))
        , m_appearance(sessionBus, "com.cutefish.Services", "/com/cutefish/Services/Appearance",
                       "com.cutefish.Services.Appearance")
        , m_privacy(dbus::Connection::system(), kPrivacyService, kPrivacyPath, kPrivacyInterface)
    {
        const auto notify = [this] { scheduleChanged(); };
        m_changedTimer.setSingleShot(true);
        m_changedTimer.setInterval(std::chrono::milliseconds(0));
        m_changedTimer.onTimeout = [this] { changed(); };

        if (m_client) {
            m_subscriptions.push_back(m_client->changed.connect([this] {
                updateActiveConnection();
                joinScanned();
                scheduleChanged();
            }));
            m_subscriptions.push_back(m_client->readyChanged.connect(notify));
        }
        m_subscriptions.push_back(m_networks->changed.connect(notify));
        m_subscriptions.push_back(m_handler->hotspotActiveChanged.connect(notify));
        m_subscriptions.push_back(m_handler->vpnFailed.connect(
                [this](const std::string &name, const std::string &message) { vpnFailed.emit(name, message); }));
        if (m_bluetooth) {
            m_subscriptions.push_back(m_bluetooth->changed.connect(notify));
            m_subscriptions.push_back(m_bluetooth->enabledChanged.connect(notify));
            m_subscriptions.push_back(m_bluetooth->adaptersChanged.connect(notify));
        }
        m_subscriptions.push_back(m_audio->rowsChanged.connect(notify));
        m_subscriptions.push_back(m_audio->dataChanged.connect(notify));
        m_subscriptions.push_back(m_audio->defaultNodeChanged.connect(notify));
        if (m_notifications)
            m_subscriptions.push_back(m_notifications->doNotDisturbChanged.connect(notify));
        m_subscriptions.push_back(m_statusBar.mpris().currentChanged.connect(notify));
        m_subscriptions.push_back(m_statusBar.mpris().currentPlayerChanged.connect(notify));
        m_subscriptions.push_back(m_statusBar.brightness().valueChanged.connect(notify));
        m_subscriptions.push_back(m_statusBar.brightness().enabledChanged.connect(notify));
        m_subscriptions.push_back(m_statusBar.battery().changed.connect(notify));
        m_subscriptions.push_back(m_statusBar.powerProfiles().profilesChanged.connect(notify));
        m_subscriptions.push_back(m_statusBar.powerProfiles().activeProfileChanged.connect(notify));

        m_matches.push_back(m_privacy.onSignal("cameraEnabledChanged", [this](const dbus::Message &message) {
            bool enabled = true;
            if (message.read(enabled) && enabled != m_cameraEnabled) {
                m_cameraEnabled = enabled;
                scheduleChanged();
            }
        }));
        m_matches.push_back(m_privacy.onSignal("microphoneEnabledChanged", [this](const dbus::Message &message) {
            bool enabled = true;
            if (message.read(enabled) && enabled != m_microphoneEnabled) {
                m_microphoneEnabled = enabled;
                scheduleChanged();
            }
        }));
        m_privacyState = base::runInThread(
                [] {
                    std::error_code error;
                    const std::string directory = kPrivacyState;
                    return std::pair<bool, bool>(!std::filesystem::exists(directory + "camera-off", error),
                                                 !std::filesystem::exists(directory + "microphone-off", error));
                },
                [this](std::pair<bool, bool> state) {
                    m_cameraEnabled = state.first;
                    m_microphoneEnabled = state.second;
                    scheduleChanged();
                });

        updateActiveConnection();
    }

    ~SessionServices() override
    {
        m_subscriptions.clear();
        m_matches.clear();
    }

    // --- Network ---

    bool ready() const { return m_client && m_client->ready(); }

    bool wirelessHardwareEnabled() const override
    {
        return ready() && m_client->manager("WirelessHardwareEnabled").value<bool>(false);
    }

    bool wirelessEnabled() const override
    {
        return ready() && m_client->manager("WirelessEnabled").value<bool>(false);
    }

    bool hotspotActive() const override { return m_handler->hotspotActive(); }

    std::string hotspotName() const override
    {
        nm::HotspotConfiguration *configuration = nm::HotspotConfiguration::instance();
        return configuration ? configuration->name() : std::string();
    }

    std::string wirelessName() const override { return m_wirelessName; }
    std::string wirelessIcon() const override { return m_wirelessIcon; }

    std::vector<WirelessNetwork> wirelessNetworks() const override
    {
        std::vector<WirelessNetwork> networks;
        for (const nm::NetworkItem &item : nm::listItems(m_networks->items(), nm::ConnectionType::Wireless)) {
            WirelessNetwork network;
            network.key = item.key;
            network.name = item.name;
            network.icon = item.icon;
            network.connectionPath = item.connectionPath;
            network.devicePath = item.devicePath;
            network.specificPath = item.specificPath;
            switch (item.connectionState) {
            case nm::ConnectionStatus::Activating:
                network.state = WirelessNetwork::State::Activating;
                break;
            case nm::ConnectionStatus::Activated:
                network.state = WirelessNetwork::State::Activated;
                break;
            case nm::ConnectionStatus::Deactivating:
                network.state = WirelessNetwork::State::Deactivating;
                break;
            default:
                network.state = WirelessNetwork::State::Deactivated;
                break;
            }
            switch (item.securityType) {
            case nm::SecurityType::None:
                network.security = WirelessNetwork::Security::None;
                break;
            case nm::SecurityType::Unknown:
                network.security = WirelessNetwork::Security::Unknown;
                break;
            case nm::SecurityType::StaticWep:
            case nm::SecurityType::WpaPsk:
            case nm::SecurityType::Wpa2Psk:
            case nm::SecurityType::Sae:
                network.security = WirelessNetwork::Security::Password;
                break;
            case nm::SecurityType::DynamicWep:
            case nm::SecurityType::Leap:
            case nm::SecurityType::WpaEap:
            case nm::SecurityType::Wpa2Eap:
            case nm::SecurityType::Wpa3SuiteB192:
                network.security = WirelessNetwork::Security::Enterprise;
                break;
            default:
                network.security = WirelessNetwork::Security::Other;
                break;
            }
            networks.push_back(std::move(network));
        }
        return networks;
    }

    void enableWireless(bool enable) override { m_handler->enableWireless(enable); }
    void requestScan() override { m_handler->requestScan(); }
    void stopHotspot() override { m_handler->stopHotspot(); }

    bool activateNetwork(const WirelessNetwork &network) override
    {
        if (network.state == WirelessNetwork::State::Activated) {
            m_handler->deactivateConnection(network.connectionPath, network.devicePath);
            return false;
        }
        if (!network.connectionPath.empty()) {
            m_handler->activateConnection(network.connectionPath, network.devicePath, network.specificPath);
            return false;
        }
        if (network.security == WirelessNetwork::Security::Enterprise) {
            whenScanned(network.specificPath);
            return true;
        }
        // The session's secret agent asks for the password first.
        m_handler->addAndActivateConnection(network.devicePath, network.specificPath);
        return network.security == WirelessNetwork::Security::Password;
    }

    // --- Bluetooth ---

    bool bluetoothAvailable() const override { return m_bluetooth && !m_bluetooth->adapters().empty(); }
    bool bluetoothEnabled() const override { return m_bluetooth && m_bluetooth->enabled(); }

    void setBluetoothEnabled(bool enabled) override
    {
        if (m_bluetooth)
            m_bluetooth->setEnabled(enabled);
    }

    // --- Switches ---

    void setDarkMode(bool dark) override { m_appearance.send("setDarkMode", dark); }

    bool doNotDisturb() const override { return m_notifications && m_notifications->doNotDisturb(); }

    void setDoNotDisturb(bool enabled) override
    {
        if (m_notifications)
            m_notifications->setDoNotDisturb(enabled);
    }

    bool cameraEnabled() const override { return m_cameraEnabled; }
    void setCameraEnabled(bool enabled) override { callPrivacy("setCameraEnabled", enabled); }
    bool microphoneEnabled() const override { return m_microphoneEnabled; }
    void setMicrophoneEnabled(bool enabled) override { callPrivacy("setMicrophoneEnabled", enabled); }

    std::vector<DeviceUser> deviceUsers() const override
    {
        return deviceUsersSource ? deviceUsersSource() : std::vector<DeviceUser>();
    }

    // --- Media ---

    MediaState media() const override
    {
        MediaState state;
        const MprisPlayer *player = m_statusBar.mpris().current();
        if (!player || player->playbackStatus() == MprisPlayer::PlaybackStatus::Stopped)
            return state;
        state.hasPlayer = true;
        state.playing = player->playbackStatus() == MprisPlayer::PlaybackStatus::Playing;
        state.title = player->title();
        state.identity = player->identity();
        state.artist = player->artist();
        state.artUrl = player->artUrl();
        state.canControl = player->canControl();
        state.canGoPrevious = player->canGoPrevious();
        state.canGoNext = player->canGoNext();
        state.canPlay = player->canPlay();
        state.canPause = player->canPause();
        return state;
    }

    void mediaPrevious() override
    {
        if (MprisPlayer *player = m_statusBar.mpris().current())
            player->previous();
    }

    void mediaPlay() override
    {
        if (MprisPlayer *player = m_statusBar.mpris().current())
            player->play();
    }

    void mediaPause() override
    {
        if (MprisPlayer *player = m_statusBar.mpris().current())
            player->pause();
    }

    void mediaNext() override
    {
        if (MprisPlayer *player = m_statusBar.mpris().current())
            player->next();
    }

    // --- Display ---

    bool brightnessEnabled() const override { return m_statusBar.brightness().isEnabled(); }
    int brightness() const override { return m_statusBar.brightness().value(); }
    void setBrightness(int value) override { m_statusBar.brightness().setValue(value); }

    // --- Sound ---

    std::shared_ptr<audio::AudioObject> sink() const { return m_audio->defaultNode(); }
    bool hasSink() const override { return sink() != nullptr; }

    double volume() const override
    {
        const auto node = sink();
        return node ? node->volume() : 0;
    }

    bool muted() const override
    {
        const auto node = sink();
        return node && node->muted();
    }

    void setVolume(double volume) override
    {
        if (const auto node = sink())
            node->setVolume(volume);
    }

    void setMuted(bool muted) override
    {
        if (const auto node = sink())
            node->setMuted(muted);
    }

    std::vector<AudioOutput> outputs() const override
    {
        std::vector<AudioOutput> outputs;
        for (const auto &object : m_audio->objects())
            outputs.push_back({object->index(), object->displayName(), object->isDefault()});
        return outputs;
    }

    void setDefaultOutput(uint32_t id) override
    {
        for (const auto &object : m_audio->objects()) {
            if (object->index() == id)
                object->setDefault(true);
        }
    }

    // --- Battery ---

    int chargePercent() const override { return m_statusBar.battery().chargePercent(); }
    bool onBattery() const override { return m_statusBar.battery().onBattery(); }
    std::string batteryStatus() const override { return m_statusBar.battery().statusString(); }

    std::vector<std::string> powerProfiles() const override { return m_statusBar.powerProfiles().profiles(); }
    std::string powerProfile() const override { return m_statusBar.powerProfiles().activeProfile(); }

    void setPowerProfile(const std::string &profile) override
    {
        m_statusBar.powerProfiles().setActiveProfile(profile);
    }

    void openSettings(const std::vector<std::string> &arguments) override
    {
        ApplicationLauncher::startDetached("cutefish-settings", arguments);
    }

private:
    void scheduleChanged()
    {
        if (!m_changedTimer.isActive())
            m_changedTimer.start();
    }

    void callPrivacy(const char *method, bool enabled)
    {
        m_calls.track([&](std::function<void()> finished) {
            return m_privacy.callAsync(
                    method,
                    [method = std::string(method), finished](const dbus::Reply &reply) {
                        if (reply.isError())
                            CF_WARNING(cfControlCenter) << "Privacy " << method << " failed: "
                                                        << reply.error().message;
                        finished();
                    },
                    enabled);
        });
    }

    // NM.ActiveConnection: the Wi-Fi connection that is up, the primary one if
    // there are several; a hotspot is not a network this machine is on.
    void updateActiveConnection()
    {
        std::string name;
        std::string icon;
        if (ready()) {
            const auto primary = m_client->manager("PrimaryConnection").get<dbus::ObjectPath>();
            std::string chosen;
            std::string device;
            for (const std::string &active : m_client->activeConnections()) {
                const auto devices = m_client->property(active, nm::activeInterface, "Devices")
                                             .value<std::vector<dbus::ObjectPath>>();
                if (m_client->property(active, nm::activeInterface, "Type").value<std::string>() != "802-11-wireless"
                    || m_client->property(active, nm::activeInterface, "State").value<uint32_t>()
                            != nm::ActiveActivated
                    || devices.empty()
                    || m_client->property(devices.front().path, nm::wirelessInterface, "Mode").value<uint32_t>()
                            == nm::ModeAp)
                    continue;
                if (chosen.empty() || (primary && active == primary->path)) {
                    chosen = active;
                    device = devices.front().path;
                }
            }
            if (!chosen.empty()) {
                const auto accessPoint =
                        m_client->property(device, nm::wirelessInterface, "ActiveAccessPoint").get<dbus::ObjectPath>();
                const std::string path = accessPoint ? accessPoint->path : std::string();
                const auto bytes = m_client->property(path, nm::accessPointInterface, "Ssid")
                                           .value<std::vector<uint8_t>>();
                const std::string ssid(bytes.begin(), bytes.end());
                // A hidden network's access point has no name of its own; its profile does.
                name = nm::wifi::ssidHidden(ssid)
                        ? m_client->property(chosen, nm::activeInterface, "Id").value<std::string>()
                        : nm::wifi::ssidLabel(ssid);
                icon = nm::wifi::strengthIcon(
                        int(m_client->property(path, nm::accessPointInterface, "Strength").value<uint8_t>()));
            }
        }
        m_wirelessName = name;
        m_wirelessIcon = icon;
    }

    // Handler.whenScanned: Settings' join window, once NetworkManager lists
    // the access point again.
    void whenScanned(const std::string &accessPoint)
    {
        if (!m_client)
            return;
        const std::string scanned = m_client->scannedAccessPoint(accessPoint);
        if (!scanned.empty()) {
            openSettings({"--join-wifi", scanned});
            return;
        }
        if (!m_client->remembers(accessPoint))
            return;
        PendingJoin join {accessPoint, m_client->accessPointDevice(accessPoint),
                          m_client->properties(accessPoint, nm::accessPointInterface)};
        m_handler->requestScan(m_client->property(join.device, nm::deviceInterface, "Interface").value<std::string>());
        m_pendingJoins.push_back(std::move(join));
    }

    void joinScanned()
    {
        for (auto it = m_pendingJoins.begin(); it != m_pendingJoins.end();) {
            const std::string scanned = m_client->listedAccessPoint(it->device, it->properties);
            if (scanned.empty() && m_client->remembers(it->accessPoint)) {
                ++it;
                continue;
            }
            if (!scanned.empty())
                openSettings({"--join-wifi", scanned});
            it = m_pendingJoins.erase(it);
        }
    }

    StatusBar &m_statusBar;
    NotificationSettings *m_notifications;
    nm::Client *m_client;
    std::unique_ptr<nm::Handler> m_handler;
    std::unique_ptr<nm::NetworkModel> m_networks;
    std::shared_ptr<bluez::Manager> m_bluetooth;
    std::unique_ptr<audio::AudioModel> m_audio;
    dbus::Proxy m_appearance;
    dbus::Proxy m_privacy;
    PendingCalls m_calls;
    base::Job m_privacyState;
    bool m_cameraEnabled = true;
    bool m_microphoneEnabled = true;
    std::string m_wirelessName;
    std::string m_wirelessIcon;
    std::vector<PendingJoin> m_pendingJoins;
    base::Timer m_changedTimer;
    std::vector<base::Subscription> m_subscriptions;
    std::vector<dbus::Match> m_matches;
};

std::unique_ptr<ControlCenterServices> createSessionServices(dbus::Connection *sessionBus, StatusBar &statusBar,
                                                             NotificationSettings *notifications)
{
    return std::make_unique<SessionServices>(sessionBus, statusBar, notifications);
}

} // namespace cutefish::shell
