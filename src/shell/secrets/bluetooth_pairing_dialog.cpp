// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/secrets/bluetooth_pairing_dialog.h"

#include "ui/dialog_window.h"
#include "ui/label.h"
#include "ui/text_field.h"

namespace cutefish::shell {

static constexpr int kDemiBold = 600;
static constexpr int kNormal = 400;

static std::string quoted(const std::string &name)
{
    return "“" + name + "”";
}

BluetoothPairingDialog::BluetoothPairingDialog(Shell *shell, PairingAgent *agent)
    : SecretDialog(shell)
    , m_agent(agent)
{
}

std::string BluetoothPairingDialog::text(const std::string &kind, const std::string &device)
{
    const std::string name = quoted(device);
    if (kind == "RequestConfirmation")
        return "Confirm that " + name + " shows this code.";
    if (kind == "RequestPinCode")
        return "Enter the PIN for " + name + ".";
    if (kind == "RequestPasskey")
        return "Enter the passkey shown on " + name + ".";
    if (kind == "DisplayPinCode" || kind == "DisplayPasskey")
        return "Type this code on " + name + ", then press Enter on it.";
    if (kind == "AuthorizeService")
        return "Allow " + name + " to use this Bluetooth service?";
    return "Allow " + name + " to pair with this computer?";
}

bool BluetoothPairingDialog::inputRequired() const
{
    const std::string kind = m_agent->pairingKind();
    return kind == "RequestPinCode" || kind == "RequestPasskey";
}

bool BluetoothPairingDialog::displayOnly() const
{
    const std::string kind = m_agent->pairingKind();
    return kind == "DisplayPinCode" || kind == "DisplayPasskey";
}

void BluetoothPairingDialog::build(ui::View *frame)
{
    m_code = frame->addChild<ui::Label>();
    m_code->setWrap(true);
    m_code->setWrapAnywhere(true);
    m_code->setTextAlignment(ui::Label::TextAlignment::Center);
    m_entered = frame->addChild<ui::Label>();
    m_entered->setTextAlignment(ui::Label::TextAlignment::Center);
    m_field = frame->addChild<ui::TextField>();
    m_field->onTextChanged = [this] {
        if (primaryButton())
            primaryButton()->setEnabled(!inputRequired() || !m_field->text().empty());
    };
    m_field->onAccepted = [this] { accept(); };
    // Input method hints that allow completion.
    m_field->setInputHints(ui::CompletionHint);
    m_error = frame->addChild<ui::Label>();
    m_error->setWrap(true);
    setRows({{m_code}, {m_entered}, {m_field}, {m_error}});
}

ui::View *BluetoothPairingDialog::initialFocus() const
{
    if (inputRequired())
        return m_field;
    return primaryButton();
}

void BluetoothPairingDialog::accept()
{
    // A code the manager rejects leaves the request open with errorText set.
    if (!inputRequired() || !m_field->text().empty())
        m_agent->respondPairing(true, m_field->text());
}

void BluetoothPairingDialog::update()
{
    const std::string kind = m_agent->pairingKind();
    setTitle("Bluetooth Pairing Request");
    setText(text(kind, m_agent->pairingDevice()));

    const bool service = kind == "AuthorizeService";
    m_code->setText(m_agent->pairingCode());
    m_code->setFontSize(service ? 14 : 28);
    m_code->setWeight(service ? kNormal : kDemiBold);
    m_code->setLetterSpacing(service ? 0 : 4);
    m_code->setVisible(!m_agent->pairingCode().empty());

    const int count = m_agent->pairingEntered();
    m_entered->setText(std::to_string(count) + (count == 1 ? " digit entered" : " digits entered"));
    m_entered->setColor(ui::ColorId::DisabledText);
    m_entered->setVisible(kind == "DisplayPasskey" && count > 0);

    m_field->setVisible(inputRequired());
    m_field->setPlaceholder(kind == "RequestPasskey" ? "Passkey" : "PIN");
    m_field->setMaximumLength(kind == "RequestPasskey" ? 6 : 16);
    m_field->setInputPurpose(kind == "RequestPasskey" ? ui::TextPurpose::Digits : ui::TextPurpose::Normal);

    m_error->setText(m_agent->errorText());
    m_error->setColor(ui::ColorId::DestructiveAction);
    m_error->setVisible(!m_agent->errorText().empty());

    const bool decline = kind == "RequestConfirmation" || kind == "RequestAuthorization" || service;
    cancelButton()->setText(decline ? "Decline" : "Cancel");
    primaryButton()->setVisible(!displayOnly());
    primaryButton()->setText(kind == "RequestConfirmation" ? "Confirm" : inputRequired() ? "Pair" : "Allow");
    primaryButton()->setEnabled(!inputRequired() || !m_field->text().empty());
}

} // namespace cutefish::shell
