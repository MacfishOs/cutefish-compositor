// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "backend/headless/headless_backend.h"

#include "compositor/compositor.h"
#include "compositor/output.h"
#include "wayland/server_util.h"

namespace cutefish::backend {

HeadlessBackend::HeadlessBackend(compositor::Compositor *compositor, int outputCount,
                                 QObject *parent)
    : QObject(parent)
    , m_compositor(compositor)
    , m_outputCount(outputCount)
{
}

std::vector<std::unique_ptr<compositor::Output>>
HeadlessBackend::createOutputs(compositor::Output *primary)
{
    primary->pixelSize = outputSize();
    primary->physicalSizeMm = QSize(509, 286);
    primary->model = QStringLiteral("headless");
    primary->name = QStringLiteral("Cutefish-1");
    primary->refreshMilliHz = 60000;
    primary->preferred = true;

    std::vector<std::unique_ptr<compositor::Output>> additional;
    for (int index = 1; index < m_outputCount; ++index) {
        auto model = std::make_unique<compositor::Output>();
        model->pixelSize = primary->pixelSize;
        model->physicalSizeMm = primary->physicalSizeMm;
        model->refreshMilliHz = primary->refreshMilliHz;
        model->preferred = true;
        model->name = QStringLiteral("Cutefish-%1").arg(index + 1);
        model->model = QStringLiteral("headless-%1").arg(index + 1);
        additional.push_back(std::move(model));
    }
    return additional;
}

void HeadlessBackend::startClock()
{
    connect(&m_clock, &QTimer::timeout, this, [this] {
        const quint64 generation = ++m_generation;
        emit aboutToPresent();
        m_compositor->frameSubmitted(generation);
        m_compositor->frameDrawn(wayland::nowInMilliseconds(), generation);
    });
    m_clock.start(16);
}

} // namespace cutefish::backend
