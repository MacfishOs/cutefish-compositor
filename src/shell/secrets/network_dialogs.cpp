// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/secrets/network_dialogs.h"

#include <algorithm>
#include <map>
#include <memory>

#include "base/event_loop.h"
#include "core/main_loop.h"
#include "ui/check_box.h"
#include "ui/dialog_window.h"
#include "ui/text_field.h"

namespace cutefish::shell {

static bool printable(const std::string &text)
{
    return std::all_of(text.begin(), text.end(), [](char c) { return c >= 0x20 && c <= 0x7e; });
}

static bool hexadecimal(const std::string &text)
{
    return std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}

// The password mask character, U+25CF BLACK CIRCLE.
static constexpr const char *kPasswordCharacter = "\u25cf";

// Sensitive data without word prediction, for a shown password.
static constexpr uint32_t kSecretHints = ui::SensitiveDataHint;

static std::string quoted(const std::string &name)
{
    return "“" + name + "”";
}

// --- WirelessPasswordDialog ---

WirelessPasswordDialog::WirelessPasswordDialog(Shell *shell, NetworkSecretAgent *agent)
    : SecretDialog(shell)
    , m_agent(agent)
{
}

std::string WirelessPasswordDialog::title(NetworkSecretAgent::Kind kind)
{
    switch (kind) {
    case NetworkSecretAgent::Kind::PrivateKeyPassword:
        return "Private Key Password Required";
    case NetworkSecretAgent::Kind::WepKey:
    case NetworkSecretAgent::Kind::WpaPassword:
        return "Wi-Fi Password Required";
    default:
        return "Network Authentication Required";
    }
}

std::string WirelessPasswordDialog::text(const NetworkSecretAgent &agent)
{
    const std::string name = quoted(agent.connectionName());
    if (agent.retry())
        return "The password for " + name + " was not accepted. Try again.";
    if (agent.kind() == NetworkSecretAgent::Kind::WepKey)
        return "Enter the WEP key for " + name + ".";
    if (!agent.identity().empty())
        return "Enter the password of " + agent.identity() + " for " + name + ".";
    return "Enter the password for " + name + ".";
}

bool WirelessPasswordDialog::acceptable(NetworkSecretAgent::Kind kind, const std::string &text)
{
    const size_t length = text.size();
    switch (kind) {
    case NetworkSecretAgent::Kind::WepKey:
        return (printable(text) && (length == 5 || length == 13))
               || (hexadecimal(text) && (length == 10 || length == 26));
    case NetworkSecretAgent::Kind::WpaPassword:
        return (printable(text) && length >= 8 && length <= 63) || (hexadecimal(text) && length == 64);
    default:
        return !text.empty() && text.find('\n') == std::string::npos;
    }
}

bool WirelessPasswordDialog::intermediate(NetworkSecretAgent::Kind kind, const std::string &text)
{
    // What a RegularExpressionValidator finds Intermediate or Acceptable: a
    // start of something the expression matches.
    const size_t length = text.size();
    switch (kind) {
    case NetworkSecretAgent::Kind::WepKey:
        return printable(text) && (length <= 13 || (hexadecimal(text) && length <= 26));
    case NetworkSecretAgent::Kind::WpaPassword:
        return printable(text) && (length <= 63 || (hexadecimal(text) && length == 64));
    default:
        return text.find('\n') == std::string::npos;
    }
}

void WirelessPasswordDialog::build(ui::View *frame)
{
    m_field = frame->addChild<ui::TextField>();
    m_field->setEchoMode(ui::TextField::EchoMode::Password);
    m_field->setPasswordCharacter(kPasswordCharacter);
    m_field->setInputHints(kSecretHints);
    m_field->setSelectByMouse(true);
    m_field->setValidator([this](const std::string &text) { return intermediate(m_agent->kind(), text); });
    m_field->onTextChanged = [this] {
        if (primaryButton())
            primaryButton()->setEnabled(acceptable(m_agent->kind(), m_field->text()));
    };
    m_field->onAccepted = [this] { accept(); };

    m_show = frame->addChild<ui::CheckBox>("Show password");
    m_show->onToggled = [this](bool checked) {
        m_field->setEchoMode(checked ? ui::TextField::EchoMode::Normal : ui::TextField::EchoMode::Password);
    };

    setRows({{m_field}, {m_show, kSpacing - 8, false}});
}

ui::View *WirelessPasswordDialog::initialFocus() const
{
    return m_field;
}

void WirelessPasswordDialog::accept()
{
    if (m_field && acceptable(m_agent->kind(), m_field->text()))
        m_agent->respond(m_field->text());
}

void WirelessPasswordDialog::update()
{
    const NetworkSecretAgent::Kind kind = m_agent->kind();
    setTitle(title(kind));
    setText(text(*m_agent));
    m_field->setPlaceholder(kind == NetworkSecretAgent::Kind::WepKey ? "WEP key" : "Password");
    primaryButton()->setText("Connect");
    primaryButton()->setEnabled(acceptable(kind, m_field->text()));
}

void WirelessPasswordDialog::requestChanged()
{
    if (!isOpen())
        return;
    m_field->clear();
    m_show->setChecked(false);
    m_field->setEchoMode(ui::TextField::EchoMode::Password);
    open();
}

// --- VpnPasswordDialog ---

VpnPasswordDialog::VpnPasswordDialog(Shell *shell, NetworkSecretAgent *agent)
    : SecretDialog(shell)
    , m_agent(agent)
{
}

std::string VpnPasswordDialog::text(const NetworkSecretAgent &agent)
{
    if (!agent.message().empty())
        return agent.message();
    if (agent.retry())
        return "Signing in to " + quoted(agent.connectionName()) + " failed. Try again.";
    return "Enter the password for " + quoted(agent.connectionName()) + ".";
}

void VpnPasswordDialog::build(ui::View *frame)
{
    m_frame = frame;
    m_show = frame->addChild<ui::CheckBox>("Show password");
    m_show->onToggled = [this](bool) { applyEcho(); };
    rebuildFields();
}

void VpnPasswordDialog::rebuildFields()
{
    // Return in a field answers the request, and the next one rebuilds the
    // fields from inside that field's key handling: they go on the next turn.
    for (ui::TextField *field : m_fields)
        core::mainLoop()->post([gone = std::shared_ptr<ui::View>(m_frame->takeChild(field))] {});
    m_fields.clear();
    m_fieldInfo = m_agent->fields();

    std::vector<Row> rows;
    for (const NetworkSecretAgent::Field &info : m_fieldInfo) {
        ui::TextField *field = m_frame->addChild<ui::TextField>(info.value);
        field->setSelectByMouse(true);
        field->setPasswordCharacter(kPasswordCharacter);
        field->setInputHints(kSecretHints);
        field->setPlaceholder(info.label);
        field->onTextChanged = [this] {
            if (primaryButton())
                primaryButton()->setEnabled(complete());
        };
        field->onAccepted = [this] { accept(); };
        rows.push_back({field, rows.empty() ? kSpacing : 8});
        m_fields.push_back(field);
    }
    rows.push_back({m_show, kSpacing - 8, false});
    setRows(std::move(rows));
    applyEcho();
}

void VpnPasswordDialog::applyEcho()
{
    for (size_t i = 0; i < m_fields.size(); ++i) {
        const bool masked = m_fieldInfo[i].secret && !m_show->isChecked();
        m_fields[i]->setEchoMode(masked ? ui::TextField::EchoMode::Password : ui::TextField::EchoMode::Normal);
    }
}

bool VpnPasswordDialog::complete() const
{
    return !m_fields.empty()
           && std::all_of(m_fields.begin(), m_fields.end(), [](ui::TextField *field) { return !field->text().empty(); });
}

ui::View *VpnPasswordDialog::initialFocus() const
{
    for (ui::TextField *field : m_fields) {
        if (field->text().empty())
            return field;
    }
    return m_fields.empty() ? nullptr : m_fields.front();
}

void VpnPasswordDialog::accept()
{
    if (!complete())
        return;
    std::map<std::string, std::string> values;
    for (size_t i = 0; i < m_fields.size(); ++i)
        values[m_fieldInfo[i].key] = m_fields[i]->text();
    m_agent->respondFields(values);
}

void VpnPasswordDialog::update()
{
    setTitle("VPN Password Required");
    setText(text(*m_agent));
    primaryButton()->setText("Connect");
    primaryButton()->setEnabled(complete());
}

void VpnPasswordDialog::requestChanged()
{
    if (!isOpen())
        return;
    m_show->setChecked(false);
    rebuildFields();
    open();
}

} // namespace cutefish::shell
