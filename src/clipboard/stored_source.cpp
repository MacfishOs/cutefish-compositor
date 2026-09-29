// Copyright 2017 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.sommelier file.
//
// Upstream: chromiumos/platform2/vm_tools/sommelier/sommelier.cc (selection handlers)
// Revision: aa8a57908bdcff297e229679dc3240b1b108f57e

#include "clipboard/stored_source.h"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <utility>

#include <QObject>
#include <QSocketNotifier>
#include <QTimer>

namespace cutefish::clipboard {

void StoredSource::send(const std::string &mime, int fd)
{
    auto it = payloads.find(mime);
    if (it == payloads.end() || fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0) {
        close(fd);
        return;
    }

    struct Writer : QObject {
        int fd;
        QByteArray data;
        qsizetype offset = 0;
        QSocketNotifier notifier;
        QTimer timeout;

        Writer(int fd, QByteArray data)
            : fd(fd)
            , data(std::move(data))
            , notifier(fd, QSocketNotifier::Write)
        {
            timeout.setSingleShot(true);
            timeout.start(10000);
            connect(&timeout, &QTimer::timeout, this, &QObject::deleteLater);
            connect(&notifier, &QSocketNotifier::activated, this, [this] {
                auto count = write(this->fd, this->data.constData() + offset,
                                   this->data.size() - offset);
                if (count > 0) {
                    offset += count;
                    timeout.start(10000);
                }
                if (offset == this->data.size()
                    || (count < 0 && errno != EAGAIN && errno != EINTR)) {
                    notifier.setEnabled(false);
                    deleteLater();
                }
            });
        }

        ~Writer() override { close(fd); }
    };

    new Writer(fd, it->second);
}

} // namespace cutefish::clipboard
