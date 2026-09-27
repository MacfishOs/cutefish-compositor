// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_BACKEND_DRM_DRM_BACKEND_H_
#define CUTEFISH_BACKEND_DRM_DRM_BACKEND_H_

#include "backend/drm/drm_output.h"
#include <QObject>
#include <QString>
#include <QTimer>

#include <cstdint>
#include <memory>
#include <vector>

struct gbm_device;
class QSocketNotifier;

namespace cutefish::render {
class EglRenderer;
}

namespace cutefish::backend {

class DrmDevice;
class DrmDeviceApi;
class DrmOutput;
class DrmHotplugMonitor;
class Session;

// Owns the DRM device: discovery, KMS resources and the page-flip event source.
// The rendering card also owns secondary scanout devices and their lifetimes.
class DrmBackend : public QObject
{
    Q_OBJECT

public:
    explicit DrmBackend(Session *session, QObject *parent = nullptr);
    ~DrmBackend() override;

    // Opens `devicePath`, or the first card supporting atomic modesetting when it
    // is empty. Enables universal planes and atomic modesetting.
    bool open(const QString &devicePath = QString());

    // Builds every connected output after solving shared CRTC and plane use.
    bool createOutputs();
    bool tryNextConfigurationResources();
    bool reprobe();
    void retainFlipEvent(std::unique_ptr<DrmOutput::FlipEvent> event);

    // Creates the EGL renderer on this device's GBM device and initializes
    // every output with it.
    bool initRenderer();
    bool refreshDevices();
    QList<DrmOutput *> allOutputs(bool retiring = false) const;

    bool hasMonotonicPresentationClock() const { return m_monotonicPresentationClock; }

    int fd() const { return m_fd; }

    QString devicePath() const { return m_devicePath; }

    QString deviceKey() const;

    QString driverName() const { return m_driverName; }

    uint64_t topologyEpoch() const { return m_topologyEpoch; }

    gbm_device *gbmDevice() const { return m_renderBackend ? m_renderBackend->gbmDevice() : m_gbm; }

    bool usesForeignRenderer() const { return m_renderBackend != nullptr; }

    uint32_t format() const { return m_format; }

    Session *session() const { return m_session; }

    render::EglRenderer *renderer() const
    {
        return m_renderBackend ? m_renderBackend->renderer() : m_renderer.get();
    }

    // The ioctl surface the display path uses. Valid between open() and
    // destruction.
    DrmDeviceApi *device() const;

    DrmOutput *primaryOutput() const
    {
        const auto outputs = allOutputs();
        return outputs.isEmpty() ? nullptr : outputs.first();
    }

    const std::vector<std::unique_ptr<DrmOutput>> &outputs() const { return m_outputs; }

    const std::vector<std::unique_ptr<DrmOutput>> &retiringOutputs() const
    {
        return m_retiringOutputs;
    }

signals:
    void outputsInvalidated();

private:
    void onDrmEvent();
    void onSessionActiveChanged(bool active);
    void closeDevice();
    void retireDevice();

    static void pageFlipHandler(int fd, unsigned int sequence, unsigned int tvSec,
                                unsigned int tvUsec, unsigned int crtcId, void *userData);

    Session *m_session = nullptr;
    DrmBackend *m_renderBackend = nullptr;
    bool m_autoDevices = false;
    int m_routeAlternative = 0;
    bool m_linearAttempt = false;
    std::vector<std::unique_ptr<DrmBackend>> m_devices;
    std::vector<std::unique_ptr<DrmBackend>> m_retiredDevices;

    bool m_monotonicPresentationClock = false;
    int m_fd = -1;
    int m_deviceId = -1;
    QString m_devicePath;
    QString m_driverName;

    std::unique_ptr<DrmDevice> m_device;
    gbm_device *m_gbm = nullptr;
    uint32_t m_format = 0;

    QSocketNotifier *m_notifier = nullptr;
    std::unique_ptr<render::EglRenderer> m_renderer;
    std::vector<std::unique_ptr<DrmOutput>> m_outputs;
    std::vector<std::unique_ptr<DrmOutput>> m_retiringOutputs;
    std::unique_ptr<DrmHotplugMonitor> m_hotplugMonitor;
    uint64_t m_topologyEpoch = 0;
    std::vector<std::unique_ptr<DrmOutput::FlipEvent>> m_retiredEvents;
};

} // namespace cutefish::backend

#endif // CUTEFISH_BACKEND_DRM_DRM_BACKEND_H_
