// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_AUTH_AVATAR_PROVIDER_H
#define CUTEFISH_AUTH_AVATAR_PROVIDER_H

#include <QQuickImageProvider>

namespace cutefish::auth {

class AvatarProvider : public QQuickImageProvider
{
public:
    AvatarProvider();

    // `id` is the account picture's file path.
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};

} // namespace cutefish::auth

#endif // CUTEFISH_AUTH_AVATAR_PROVIDER_H
