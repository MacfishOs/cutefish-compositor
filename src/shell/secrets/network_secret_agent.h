// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_SHELL_SECRETS_NETWORK_SECRET_AGENT_H_
#define CUTEFISH_SHELL_SECRETS_NETWORK_SECRET_AGENT_H_

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/dbus.h"
#include "base/signal.h"
#include "base/timer.h"
#include "networkmanager/types.h"
#include "shell/secrets/vpn_secret_fields.h"

namespace cutefish::networkmanager {
class Client;
class Handler;
} // namespace cutefish::networkmanager

namespace cutefish::shell {

class SecretStore;

// The session's NetworkManager secret agent (cutefish-framework's
// NetworkSecretAgent): registered as com.cutefish.network with every
// NetworkManager that starts, it answers from the keyring and asks the user
// through the prompt bound to it when NetworkManager allows interaction.
// Typed agent-owned secrets go to the keyring, as SaveSecrets would not bring
// them back. It also serves com.cutefish.SecretAgent's ConnectWireless, which
// asks for a new network's password before anything is added.
class NetworkSecretAgent
{
public:
    enum class Kind {
        WpaPassword,
        WepKey,
        LeapPassword,
        EapPassword,
        PrivateKeyPassword,
        VpnSecrets,
    };

    // A VPN secret to ask for, with what NetworkManager or the keyring has.
    struct Field {
        std::string key;
        std::string label;
        bool secret = true;
        std::string value;
    };

    // `client` is NetworkManager's on the system bus, where the agent object
    // is exported; `session` serves com.cutefish.SecretAgent (after
    // registerService()) and reaches the keyring. Either may be null.
    NetworkSecretAgent(networkmanager::Client *client, base::dbus::Connection *session);
    ~NetworkSecretAgent();

    NetworkSecretAgent(const NetworkSecretAgent &) = delete;
    NetworkSecretAgent &operator=(const NetworkSecretAgent &) = delete;

    // Exports /SecretAgent on the session bus and owns com.cutefish.SecretAgent.
    bool registerService();

    // The request at the head of the queue, which the prompt shows.
    bool pending() const { return !m_requests.empty(); }
    std::string connectionName() const;
    Kind kind() const;
    std::string identity() const;
    bool retry() const;
    std::vector<Field> fields() const;
    // What a VPN's server said, if anything.
    std::string message() const;

    void respond(const std::string &secret);
    // A VPN's secrets by key.
    void respondFields(const std::map<std::string, std::string> &values);
    void cancel();

    // com.cutefish.SecretAgent.ConnectWireless: asks for the password of the
    // access point, and joins it with a new profile once given.
    void connectWireless(const std::string &devicePath, const std::string &accessPointPath,
                         const std::string &name, bool wep);

    // A request with no one waiting for the answer, put at the head of the
    // queue: for rendering the prompts without NetworkManager.
    struct Prompt {
        Kind kind = Kind::WpaPassword;
        std::string connectionName;
        std::string identity;
        bool retry = false;
        std::vector<Field> fields;
        std::string message;
    };
    void showForTesting(const Prompt &prompt);

    // The head of the queue changed: a prompt to show, change or close.
    base::Signal<> requestChanged;

private:
    struct Request {
        uint64_t id = 0;
        std::optional<base::dbus::MethodCall> call;
        std::string connectionPath;
        std::string settingName;
        std::string connectionName;
        std::string uuid;
        std::string identity;
        std::string key;
        uint32_t secretFlags = 0;
        Kind kind = Kind::WpaPassword;
        bool interactive = false;
        bool retry = false;
        bool noErrors = false;
        // A VPN asks for several secrets, which its data says how to keep.
        std::vector<VpnSecretField> fields;
        std::map<std::string, std::string> vpnData;
        std::map<std::string, std::string> values;
        std::string serverMessage;
        // A network asked about before anything is added, see connectWireless().
        std::string devicePath;
        std::string accessPointPath;
        std::string deviceActive;
    };

    void exportAgent();
    void readyChanged();
    void registerAgent(int attempt = 0);
    bool fromNetworkManager(const base::dbus::Message &message) const;
    void getSecrets(base::dbus::MethodCall &call);
    void getVpnSecrets(Request request);
    void cancelGetSecrets(const std::string &connectionPath, const std::string &settingName);
    void saveSecrets(base::dbus::MethodCall &call);
    void deleteSecrets(base::dbus::MethodCall &call);

    void reply(const Request &request, const networkmanager::Settings &secrets);
    void replyVpn(const Request &request, const std::map<std::string, std::string> &values);
    void fail(const Request &request, const std::string &error, const std::string &text);
    void ask(const Request &request);
    void finish();
    void dismissStalePrompts();

    networkmanager::Client *m_client;
    base::dbus::Connection *m_session;
    std::unique_ptr<SecretStore> m_store;
    std::unique_ptr<networkmanager::Handler> m_handler;
    std::unique_ptr<base::dbus::Object> m_agentObject;
    std::unique_ptr<base::dbus::Object> m_promptObject;
    std::vector<Request> m_lookups;
    std::deque<Request> m_requests;
    uint64_t m_nextId = 0;
    uint64_t m_registration = 0;
    bool m_registered = false;
    base::Timer m_registerRetry;
    base::dbus::PendingCall m_registerCall;
    base::Subscription m_readySubscription;
    base::Subscription m_changedSubscription;
    std::shared_ptr<char> m_alive = std::make_shared<char>();
};

} // namespace cutefish::shell

#endif // CUTEFISH_SHELL_SECRETS_NETWORK_SECRET_AGENT_H_
