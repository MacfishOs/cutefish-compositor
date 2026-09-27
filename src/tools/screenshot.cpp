// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QSaveFile>
#include <QTransform>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ext-image-capture-source-v1-client-protocol.h>
#include <ext-image-copy-capture-v1-client-protocol.h>
#include <functional>
#include <memory>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>
#include <wayland-client.h>

namespace {
struct Output {
    wl_output *proxy = nullptr;
    QString name;
};

struct Capture {
    wl_display *display = nullptr;
    wl_registry *registry = nullptr;
    wl_shm *shm = nullptr;
    ext_output_image_capture_source_manager_v1 *sources = nullptr;
    ext_image_copy_capture_manager_v1 *copies = nullptr;
    ext_image_capture_source_v1 *source = nullptr;
    ext_image_copy_capture_session_v1 *session = nullptr;
    ext_image_copy_capture_frame_v1 *frame = nullptr;
    wl_buffer *buffer = nullptr;
    std::vector<std::unique_ptr<Output>> outputs;
    QSize size;
    bool argb = false;
    bool constraints = false;
    bool finished = false;
    bool ready = false;
    uint32_t transform = 0;
    QString error;
    void *pixels = MAP_FAILED;
    size_t length = 0;

    ~Capture()
    {
        if (frame)
            ext_image_copy_capture_frame_v1_destroy(frame);
        if (buffer)
            wl_buffer_destroy(buffer);
        if (session)
            ext_image_copy_capture_session_v1_destroy(session);
        if (source)
            ext_image_capture_source_v1_destroy(source);
        if (copies)
            ext_image_copy_capture_manager_v1_destroy(copies);
        if (sources)
            ext_output_image_capture_source_manager_v1_destroy(sources);
        for (auto &output : outputs)
            wl_output_destroy(output->proxy);
        if (shm)
            wl_shm_destroy(shm);
        if (registry)
            wl_registry_destroy(registry);
        if (display)
            wl_display_disconnect(display);
        if (pixels != MAP_FAILED)
            munmap(pixels, length);
    }

    bool wait(const std::function<bool()> &done)
    {
        QElapsedTimer timer;
        timer.start();
        while (!done() && !finished) {
            if (wl_display_dispatch_pending(display) < 0)
                return false;
            if (done() || finished)
                break;
            if (wl_display_flush(display) < 0 && errno != EAGAIN)
                return false;
            pollfd fd {wl_display_get_fd(display), POLLIN, 0};
            const int remaining = 5000 - int(timer.elapsed());
            if (remaining <= 0) {
                error = "Capture timed out";
                return false;
            }
            const int result = poll(&fd, 1, remaining);
            if (result < 0 && errno == EINTR)
                continue;
            if (result <= 0 || (fd.revents & (POLLERR | POLLHUP)))
                return false;
            if (wl_display_dispatch(display) < 0)
                return false;
        }
        return done();
    }

    bool sync()
    {
        bool done = false;
        auto *callback = wl_display_sync(display);
        static const wl_callback_listener listener = {[](void *data, wl_callback *, uint32_t) {
            *static_cast<bool *>(data) = true;
        }};
        wl_callback_add_listener(callback, &listener, &done);
        const bool result = wait([&] {
            return done;
        });
        wl_callback_destroy(callback);
        return result;
    }
};

const wl_output_listener outputListener = {
        [](void *, wl_output *, int32_t, int32_t, int32_t, int32_t, int32_t, const char *,
           const char *, int32_t) {},
        [](void *, wl_output *, uint32_t, int32_t, int32_t, int32_t) {},
        [](void *, wl_output *) {},
        [](void *, wl_output *, int32_t) {},
        [](void *data, wl_output *, const char *name) {
            static_cast<Output *>(data)->name = QString::fromUtf8(name);
        },
        [](void *, wl_output *, const char *) {}};

const wl_registry_listener registryListener = {
        [](void *data, wl_registry *registry, uint32_t name, const char *interface,
           uint32_t version) {
            auto &capture = *static_cast<Capture *>(data);
            if (!strcmp(interface, wl_output_interface.name)) {
                auto output = std::make_unique<Output>();
                output->proxy = static_cast<wl_output *>(wl_registry_bind(
                        registry, name, &wl_output_interface, std::min(version, 4u)));
                wl_output_add_listener(output->proxy, &outputListener, output.get());
                capture.outputs.push_back(std::move(output));
            } else if (!strcmp(interface, wl_shm_interface.name)) {
                capture.shm = static_cast<wl_shm *>(
                        wl_registry_bind(registry, name, &wl_shm_interface, 1));
            } else if (!strcmp(interface,
                               ext_output_image_capture_source_manager_v1_interface.name)) {
                capture.sources = static_cast<ext_output_image_capture_source_manager_v1 *>(
                        wl_registry_bind(registry, name,
                                         &ext_output_image_capture_source_manager_v1_interface, 1));
            } else if (!strcmp(interface, ext_image_copy_capture_manager_v1_interface.name)) {
                capture.copies = static_cast<ext_image_copy_capture_manager_v1 *>(wl_registry_bind(
                        registry, name, &ext_image_copy_capture_manager_v1_interface, 1));
            }
        },
        [](void *, wl_registry *, uint32_t) {}};

const ext_image_copy_capture_session_v1_listener sessionListener = {
        [](void *data, ext_image_copy_capture_session_v1 *, uint32_t w, uint32_t h) {
            auto &capture = *static_cast<Capture *>(data);
            capture.size = w > 0 && h > 0 && w <= 16384 && h <= 16384 ? QSize(w, h) : QSize();
            capture.argb = false;
        },
        [](void *data, ext_image_copy_capture_session_v1 *, uint32_t format) {
            if (format == WL_SHM_FORMAT_ARGB8888)
                static_cast<Capture *>(data)->argb = true;
        },
        [](void *, ext_image_copy_capture_session_v1 *, wl_array *) {},
        [](void *, ext_image_copy_capture_session_v1 *, uint32_t, wl_array *) {},
        [](void *data, ext_image_copy_capture_session_v1 *) {
            static_cast<Capture *>(data)->constraints = true;
        },
        [](void *data, ext_image_copy_capture_session_v1 *) {
            auto &capture = *static_cast<Capture *>(data);
            capture.finished = true;
            capture.error = "Capture source stopped";
        }};

const ext_image_copy_capture_frame_v1_listener frameListener = {
        [](void *data, ext_image_copy_capture_frame_v1 *, uint32_t transform) {
            static_cast<Capture *>(data)->transform = transform;
        },
        [](void *, ext_image_copy_capture_frame_v1 *, int32_t, int32_t, int32_t, int32_t) {},
        [](void *, ext_image_copy_capture_frame_v1 *, uint32_t, uint32_t, uint32_t) {},
        [](void *data, ext_image_copy_capture_frame_v1 *) {
            auto &capture = *static_cast<Capture *>(data);
            capture.ready = capture.finished = true;
        },
        [](void *data, ext_image_copy_capture_frame_v1 *, uint32_t reason) {
            auto &capture = *static_cast<Capture *>(data);
            capture.error = QStringLiteral("Capture failed (reason %1)").arg(reason);
            capture.finished = true;
        }};
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription("Capture a Wayland output to PNG.");
    parser.addHelpOption();
    parser.addOption({"list", "List output names."});
    parser.addOption({"output", "Output to capture (required with multiple outputs).", "name"});
    parser.addOption({"cursor", "Include the pointer cursor."});
    parser.addPositionalArgument("file", "Destination PNG file.", "[file]");
    parser.process(app);
    Capture capture;
    const auto fail = [&](const QString &message) {
        fprintf(stderr, "%s\n", qPrintable(message));
        return 1;
    };
    capture.display = wl_display_connect(nullptr);
    if (!capture.display)
        return fail("Cannot connect to Wayland display");
    capture.registry = wl_display_get_registry(capture.display);
    wl_registry_add_listener(capture.registry, &registryListener, &capture);
    if (!capture.sync() || !capture.sync())
        return fail("Cannot enumerate outputs");
    if (parser.isSet("list")) {
        for (const auto &output : capture.outputs)
            printf("%s\n", qPrintable(output->name));
        return 0;
    }
    if (parser.positionalArguments().size() != 1)
        return fail("Specify one destination PNG file");
    if (!capture.sources || !capture.copies || !capture.shm)
        return fail("Output capture is unavailable or this executable is not authorized");
    Output *output = nullptr;
    for (auto &candidate : capture.outputs) {
        if (candidate->name == parser.value("output")
            || (!parser.isSet("output") && capture.outputs.size() == 1))
            output = candidate.get();
    }
    if (!output)
        return fail("Select an output with --output NAME; use --list to see names");
    capture.source = ext_output_image_capture_source_manager_v1_create_source(capture.sources,
                                                                              output->proxy);
    capture.session = ext_image_copy_capture_manager_v1_create_session(
            capture.copies, capture.source,
            parser.isSet("cursor") ? EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS : 0);
    ext_image_copy_capture_session_v1_add_listener(capture.session, &sessionListener, &capture);
    if (!capture.wait([&] {
            return capture.constraints;
        })
        || capture.size.isEmpty() || !capture.argb)
        return fail(capture.error.isEmpty() ? "No supported SHM capture format" : capture.error);
    const QSize bufferSize = capture.size;
    capture.length = size_t(bufferSize.width()) * bufferSize.height() * 4;
    const int fd = memfd_create("cutefish-screenshot", MFD_CLOEXEC);
    if (fd < 0)
        return fail("Cannot allocate screenshot buffer");
    if (ftruncate(fd, capture.length) != 0) {
        close(fd);
        return fail("Cannot size screenshot buffer");
    }
    capture.pixels = mmap(nullptr, capture.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (capture.pixels == MAP_FAILED) {
        close(fd);
        return fail("Cannot map screenshot buffer");
    }
    auto *pool = wl_shm_create_pool(capture.shm, fd, capture.length);
    close(fd);
    capture.buffer = wl_shm_pool_create_buffer(pool, 0, bufferSize.width(), bufferSize.height(),
                                               bufferSize.width() * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    capture.frame = ext_image_copy_capture_session_v1_create_frame(capture.session);
    ext_image_copy_capture_frame_v1_add_listener(capture.frame, &frameListener, &capture);
    ext_image_copy_capture_frame_v1_attach_buffer(capture.frame, capture.buffer);
    ext_image_copy_capture_frame_v1_damage_buffer(capture.frame, 0, 0, bufferSize.width(),
                                                  bufferSize.height());
    ext_image_copy_capture_frame_v1_capture(capture.frame);
    if (!capture.wait([&] {
            return capture.ready;
        }))
        return fail(capture.error.isEmpty() ? "Capture did not complete" : capture.error);
    QImage image(static_cast<const uchar *>(capture.pixels), bufferSize.width(),
                 bufferSize.height(), bufferSize.width() * 4, QImage::Format_ARGB32_Premultiplied);
    QTransform transform;
    if (capture.transform & 4)
        transform.scale(-1, 1);
    transform.rotate(90 * int(capture.transform & 3));
    image = image.transformed(transform);
    QSaveFile file(parser.positionalArguments().front());
    if (!file.open(QIODevice::WriteOnly) || !image.save(&file, "PNG") || !file.commit())
        return fail("Cannot save PNG");
    return 0;
}
