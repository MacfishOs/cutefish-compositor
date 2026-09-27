// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "auth/greetd_backend.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>

namespace cutefish::auth {
GreetdBackend::GreetdBackend(QObject *parent)
    : AuthBackend(parent)
    , m_socket(new QLocalSocket(this))
{
    connect(m_socket, &QLocalSocket::readyRead, this, &GreetdBackend::readResponses);
    connect(m_socket, &QLocalSocket::errorOccurred, this, &GreetdBackend::fail);
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        m_socket->abort();
        fail();
    });
}

GreetdBackend::~GreetdBackend()
{
    if (m_sessionOpen && m_phase != Phase::Complete)
        send({{QStringLiteral("type"), QStringLiteral("cancel_session")}});
}

void GreetdBackend::start()
{
    const QString path = qEnvironmentVariable("GREETD_SOCK");
    if (path.isEmpty()) {
        fail();
        return;
    }
    m_socket->connectToServer(path);
}

void GreetdBackend::authenticate(const QString &userName, Secret secret)
{
    if (m_phase == Phase::Authenticating || m_phase == Phase::Starting
        || m_phase == Phase::Complete)
        return;
    if (userName.isEmpty() || secret.isEmpty()
        || m_socket->state() != QLocalSocket::ConnectedState) {
        fail();
        return;
    }
    m_userName = userName;
    m_pending = std::move(secret);
    m_retry = true;
    if (m_phase == Phase::Cancelling)
        return;
    if (m_sessionOpen) {
        m_phase = Phase::Cancelling;
        send({{QStringLiteral("type"), QStringLiteral("cancel_session")}});
    } else {
        createSession();
    }
}

void GreetdBackend::createSession()
{
    m_retry = false;
    m_sessionOpen = true;
    m_phase = Phase::Authenticating;
    send({{QStringLiteral("type"), QStringLiteral("create_session")},
          {QStringLiteral("username"), m_userName}});
}

void GreetdBackend::reset()
{
    m_pending.clear();
    m_retry = false;
    if (m_phase == Phase::Idle && m_sessionOpen) {
        m_phase = Phase::Cancelling;
        send({{QStringLiteral("type"), QStringLiteral("cancel_session")}});
    }
}

void GreetdBackend::send(const QJsonObject &request)
{
    if (m_socket->state() != QLocalSocket::ConnectedState) {
        fail();
        return;
    }
    QByteArray payload = QJsonDocument(request).toJson(QJsonDocument::Compact);
    const quint32 length = quint32(payload.size());
    m_socket->write(reinterpret_cast<const char *>(&length), sizeof(length));
    m_socket->write(payload);
    explicit_bzero(payload.data(), size_t(payload.size()));
    m_socket->flush();
    m_timeout.start(30000);
}

void GreetdBackend::readResponses()
{
    m_buffer.append(m_socket->readAll());
    while (m_buffer.size() >= 4) {
        quint32 length;
        memcpy(&length, m_buffer.constData(), 4);
        if (length > 65536) {
            m_socket->abort();
            fail();
            return;
        }
        if (m_buffer.size() < 4 + qsizetype(length))
            return;
        const auto document = QJsonDocument::fromJson(m_buffer.mid(4, length));
        m_buffer.remove(0, 4 + length);
        m_timeout.stop();
        if (!document.isObject()) {
            fail();
            return;
        }
        handleResponse(document.object());
    }
}

void GreetdBackend::handleResponse(const QJsonObject &response)
{
    const QString type = response.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("error")) {
        fail();
        return;
    }
    if (type == QLatin1String("success")) {
        if (m_phase == Phase::Cancelling) {
            m_sessionOpen = false;
            m_phase = Phase::Idle;
            if (m_retry)
                createSession();
        } else if (m_phase == Phase::Authenticating) {
            m_pending.clear();
            m_phase = Phase::Starting;
            // The command and environment are for greetd; cutefish-dm starts
            // the session its id names and sets the environment itself.
            QJsonObject start {
                    {QStringLiteral("type"), QStringLiteral("start_session")},
                    {QStringLiteral("cmd"),
                     QJsonArray::fromStringList(m_session.command.isEmpty()
                                                        ? QStringList {QStringLiteral("cutefish-session")}
                                                        : m_session.command)},
                    {QStringLiteral("env"), QJsonArray::fromStringList(m_session.environment)}};
            if (!m_session.id.isEmpty())
                start.insert(QStringLiteral("session"), m_session.id);
            send(start);
        } else if (m_phase == Phase::Starting) {
            m_phase = Phase::Complete;
            emit succeeded();
        }
        return;
    }
    if (type != QLatin1String("auth_message") || m_phase != Phase::Authenticating) {
        fail();
        return;
    }
    const QString kind = response.value(QStringLiteral("auth_message_type")).toString();
    QJsonValue answer;
    if (kind == QLatin1String("secret") && !m_pending.isEmpty()) {
        answer = QString::fromUtf8(m_pending.constData(), m_pending.size());
        m_pending.clear();
    } else if (kind == QLatin1String("visible")) {
        answer = m_userName;
    } else if (kind != QLatin1String("info") && kind != QLatin1String("error")) {
        fail();
        return;
    }
    send({{QStringLiteral("type"), QStringLiteral("post_auth_message_response")},
          {QStringLiteral("response"), answer}});
}

void GreetdBackend::fail()
{
    m_timeout.stop();
    m_pending.clear();
    m_retry = false;
    if (m_phase == Phase::Complete)
        return;
    m_phase = Phase::Idle;
    emit failed(tr("Sign-in failed"));
}
} // namespace cutefish::auth
