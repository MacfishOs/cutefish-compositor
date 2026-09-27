// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_AUTH_SECRET_H
#define CUTEFISH_AUTH_SECRET_H

#include <QString>

#include <cstring>
#include <utility>
#include <vector>

namespace cutefish::auth {

inline void wipeString(QString &secret)
{
    if (!secret.isEmpty()) {
        QChar *data = secret.data();
        const qsizetype size = secret.size();
        for (qsizetype i = 0; i < size; ++i)
            data[i] = QChar(u'\0');
    }
    secret.clear();
}

class Secret
{
public:
    Secret() = default;

    explicit Secret(const QString &text)
    {
        QByteArray utf8 = text.toUtf8();
        m_data.assign(utf8.constData(), utf8.constData() + utf8.size());
        m_data.push_back('\0');
        explicit_bzero(utf8.data(), size_t(utf8.size()));
    }

    Secret(const Secret &) = delete;
    Secret &operator=(const Secret &) = delete;

    Secret(Secret &&other) noexcept
        : m_data(std::move(other.m_data))
    {
        other.m_data.clear();
    }

    Secret &operator=(Secret &&other) noexcept
    {
        if (this != &other) {
            clear();
            m_data = std::move(other.m_data);
            other.m_data.clear();
        }
        return *this;
    }

    ~Secret() { clear(); }

    bool isEmpty() const { return m_data.size() <= 1; }

    // NUL-terminated, for the C APIs that take a credential. Never valid after
    // clear() and never copied into anything that outlives this object.
    const char *constData() const { return m_data.empty() ? "" : m_data.data(); }

    qsizetype size() const { return m_data.empty() ? 0 : qsizetype(m_data.size() - 1); }

    void clear()
    {
        if (!m_data.empty()) {
            // volatile so the write is not optimised away as dead.
            volatile char *data = m_data.data();
            for (size_t i = 0; i < m_data.size(); ++i)
                data[i] = '\0';
        }
        m_data.clear();
    }

private:
    std::vector<char> m_data;
};

} // namespace cutefish::auth

#endif // CUTEFISH_AUTH_SECRET_H
