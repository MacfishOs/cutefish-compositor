// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_BACKEND_HEADLESS_BACKEND_H_
#define CUTEFISH_BACKEND_HEADLESS_BACKEND_H_

#include <memory>
#include <vector>

#include <QObject>
#include <QSize>
#include <QTimer>

namespace cutefish {

namespace compositor {
class Compositor;
struct Output;
} // namespace compositor

namespace backend {

// Stands in for a display: synthesizes outputs and a presentation clock so the
// compositor and real toolkit clients run with no hardware.
//
// This is deliberately not a DrmBackend sibling. There is no shared interface to
// implement: DisplayRuntime and the topology coordinator take a DrmBackend and
// already treat a null one as "no hardware", which is exactly what headless is.
class HeadlessBackend : public QObject
{
    Q_OBJECT

public:
    static QSize outputSize() { return QSize(1920, 1080); }

    HeadlessBackend(compositor::Compositor *compositor, int outputCount, QObject *parent = nullptr);

    // Describes the primary output and returns any further ones. A Qt or GTK
    // client with no output has no screen to place itself on, so headless mode
    // would be untestable with real toolkit clients without these.
    std::vector<std::unique_ptr<compositor::Output>> createOutputs(compositor::Output *primary);

    // Starts the substitute vblank. Nothing presents frames without a display,
    // and a client that never gets a frame callback stops drawing after its
    // first buffer, so a plain 60 Hz timer stands in for the flip.
    void startClock();

signals:
    // Emitted before the compositor is told, so session state settles first.
    void aboutToPresent();

private:
    compositor::Compositor *const m_compositor;
    const int m_outputCount;
    QTimer m_clock;
    quint64 m_generation = 0;
};

} // namespace backend
} // namespace cutefish

#endif // CUTEFISH_BACKEND_HEADLESS_BACKEND_H_
