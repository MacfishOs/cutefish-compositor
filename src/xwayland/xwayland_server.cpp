// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xwayland_server.h"

#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/close_range.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <QDataStream>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSocketNotifier>

#include "compositor/surface.h"
#include "core/process_environment.h"
#include "wayland/server.h"
#include "wayland/server_util.h"
#include "wm/window_manager.h"
#include "xwayland_keyboard_grab.h"
#include "xwayland_shell.h"
#include "xwm.h"

namespace cutefish::xwayland {

XwaylandServer::XwaylandServer(wl_display* display, wayland::PrivilegedClients* privileged,
                               wm::WindowManager* manager)
    : m_display(display)
    , m_privileged(privileged)
    , m_manager(manager)
{
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        fail(QStringLiteral("startup timeout"));
    });
    // One crash reports errorOccurred and finished, and closes both connections:
    // deferred failures of one epoch count as one, and QProcess is not
    // restarted from inside its own signal.
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (!m_stopping)
            deferFailure(m_process.errorString());
    });
    connect(&m_process, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        if (!m_stopping)
            deferFailure(QStringLiteral("Xwayland exited"));
    });
}

XwaylandServer::~XwaylandServer()
{
    stop();
}

bool XwaylandServer::start(const Options& options)
{
    stop();
    m_error.clear();
    m_options = options;
    m_restarts.clear();
    m_readinessFile = options.readinessFile;
    m_runtime = std::make_unique<QTemporaryDir>(qEnvironmentVariable("XDG_RUNTIME_DIR", "/tmp") +
                                                "/cutefish-xwayland-XXXXXX");
    if (!m_runtime->isValid()) {
        fail(QStringLiteral("cannot create private runtime directory"));
        return false;
    }
    m_authority = m_runtime->path() + "/Xauthority";
    QFile auth(m_authority);
    if (!auth.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        !auth.setPermissions(QFile::ReadOwner | QFile::WriteOwner)) {
        fail(QStringLiteral("cannot create Xauthority"));
        return false;
    }
    char cookie[16];
    if (getrandom(cookie, sizeof(cookie), 0) != sizeof(cookie)) {
        fail(QStringLiteral("cannot generate authentication cookie"));
        return false;
    }
    // FamilyWild permits displayfd allocation before the display number is known.
    QDataStream stream(&auth);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << quint16(65535) << quint16(0) << quint16(0) << quint16(18);
    stream.writeRawData("MIT-MAGIC-COOKIE-1", 18);
    stream << quint16(sizeof(cookie));
    stream.writeRawData(cookie, sizeof(cookie));
    if (stream.status() != QDataStream::Ok || !auth.flush()) {
        fail(QStringLiteral("cannot write Xauthority"));
        return false;
    }
    auth.close();
    if (!m_options.lazy)
        return launch();
    if (!openSockets()) {
        fail(QStringLiteral("cannot open X11 display sockets"));
        return false;
    }
    if (!writeReadiness())
        return false;
    m_published = true;
    qInfo().noquote() << "Xwayland on demand: DISPLAY=" + m_displayName
                      << "XAUTHORITY=" + m_authority;
    arm();
    if (ready)
        ready();
    return true;
}

// The X server's own display reservation: a lock file naming a live process,
// and a filesystem and an abstract socket, both handed to Xwayland with
// -listenfd once a client arrives.
bool XwaylandServer::openSockets()
{
    struct stat directory {};
    if (lstat("/tmp/.X11-unix", &directory) != 0 || !S_ISDIR(directory.st_mode)
        || (directory.st_uid != 0 && directory.st_uid != getuid())
        || !(directory.st_mode & S_ISVTX))
        return false;
    auto listenOn = [](const QByteArray& path, bool abstract) {
        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        if (size_t(path.size()) + 1 > sizeof(address.sun_path))
            return -1;
        std::memcpy(address.sun_path + (abstract ? 1 : 0), path.constData(), path.size());
        const socklen_t length = offsetof(sockaddr_un, sun_path) + path.size() + 1;
        const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0)
            return -1;
        if (bind(fd, reinterpret_cast<sockaddr*>(&address), length) != 0 || listen(fd, 1) != 0) {
            close(fd);
            return -1;
        }
        return fd;
    };
    for (int display = 0; display < 100; ++display) {
        const QByteArray lock = "/tmp/.X" + QByteArray::number(display) + "-lock";
        int fd = -1;
        for (int attempt = 0; attempt < 2 && fd < 0; ++attempt) {
            fd = open(lock.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0444);
            if (fd >= 0 || errno != EEXIST)
                break;
            // A lock left by a server that died is taken over, as X servers do.
            QFile stale(QString::fromLocal8Bit(lock));
            const qint64 owner = stale.open(QIODevice::ReadOnly) ? stale.readLine().trimmed().toLongLong()
                                                                 : 0;
            if (owner <= 0 || ::kill(pid_t(owner), 0) == 0 || errno != ESRCH)
                break;
            unlink(lock.constData());
        }
        if (fd < 0)
            continue;
        char pid[12];
        std::snprintf(pid, sizeof(pid), "%10d\n", int(getpid()));
        const bool written = write(fd, pid, 11) == 11;
        close(fd);
        const QByteArray path = "/tmp/.X11-unix/X" + QByteArray::number(display);
        unlink(path.constData());
        const int filesystem = written ? listenOn(path, false) : -1;
        const int abstract = filesystem >= 0 ? listenOn(path, true) : -1;
        if (abstract < 0) {
            if (filesystem >= 0) {
                close(filesystem);
                unlink(path.constData());
            }
            unlink(lock.constData());
            continue;
        }
        m_listenFds = {filesystem, abstract};
        m_lockFile = QString::fromLocal8Bit(lock);
        m_socketFile = QString::fromLocal8Bit(path);
        m_displayName = ':' + QString::number(display);
        return true;
    }
    return false;
}

void XwaylandServer::closeSockets()
{
    m_listenNotifiers.clear();
    for (int fd : std::exchange(m_listenFds, {}))
        close(fd);
    if (!m_socketFile.isEmpty())
        unlink(QFile::encodeName(std::exchange(m_socketFile, {})).constData());
    if (!m_lockFile.isEmpty())
        unlink(QFile::encodeName(std::exchange(m_lockFile, {})).constData());
}

void XwaylandServer::arm()
{
    if (m_listenNotifiers.empty()) {
        for (int fd : m_listenFds) {
            auto notifier = std::make_unique<QSocketNotifier>(fd, QSocketNotifier::Read);
            connect(notifier.get(), &QSocketNotifier::activated, this, [this] {
                // Xwayland accepts on the same sockets from here on.
                for (auto& listener : m_listenNotifiers)
                    listener->setEnabled(false);
                if (!m_client)
                    launch();
            });
            m_listenNotifiers.push_back(std::move(notifier));
        }
    }
    for (auto& listener : m_listenNotifiers)
        listener->setEnabled(true);
}

bool XwaylandServer::launch()
{
    int wayland[2] {-1, -1}, wm[2] {-1, -1}, notify[2] {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wayland) ||
        socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wm) || pipe2(notify, O_CLOEXEC)) {
        for (int fd : {wayland[0], wayland[1], wm[0], wm[1], notify[0], notify[1]})
            if (fd >= 0)
                close(fd);
        fail(QStringLiteral("cannot create Xwayland channels"));
        return false;
    }
    m_client = wl_client_create(m_display, wayland[0]);
    if (!m_client) {
        for (int fd : {wayland[0], wayland[1], wm[0], wm[1], notify[0], notify[1]})
            close(fd);
        fail(QStringLiteral("cannot adopt Wayland channel"));
        return false;
    }
    // Before Xwayland binds anything: its outputs are sized in its own units.
    if (auto* server = wayland::Server::serverForDisplay(m_display))
        server->setClientScale(m_client, m_scale);
    m_destroy.owner = this;
    m_destroy.listener.notify = [](wl_listener* listener, void*) {
        auto* self = reinterpret_cast<Listener*>(listener)->owner;
        wl_list_remove(&self->m_destroy.listener.link);
        if (auto* server = wayland::Server::serverForDisplay(self->m_display))
            server->setClientScale(self->m_client, 1);
        self->m_client = nullptr;
        if (self->m_shell)
            self->m_shell->revokeClient();
        if (!self->m_stopping)
            self->deferFailure(QStringLiteral("Wayland connection lost"));
    };
    wl_client_add_destroy_listener(m_client, &m_destroy.listener);
    m_association = std::make_unique<SurfaceAssociation>();
    m_shell =
        std::make_unique<XwaylandShell>(m_display, m_privileged, m_client, m_association.get());
    m_keyboardGrab = std::make_unique<XwaylandKeyboardGrab>(m_display, m_privileged, m_client,
                                                            m_manager->seat());
    m_shell->bound = [this] {
        publish();
    };
    m_xwm = std::make_unique<Xwm>(m_manager, m_association.get());
    m_xwm->setScale(m_scale);
    m_xwm->setPingTimeout(m_options.pingTimeoutMs);
    m_xwm->setWindowLimit(m_options.windowLimit);
    m_xwm->initialized = [this](bool ok) {
        if (!ok) {
            deferFailure(QStringLiteral("XWM initialization failed"));
            return;
        }
        m_xwmReady = true;
        publish();
    };
    m_xwm->failed = [this] {
        deferFailure(QStringLiteral("XWM connection lost"));
    };
    m_readFd = notify[0];
    fcntl(m_readFd, F_SETFL, O_NONBLOCK);
    m_readNotifier = new QSocketNotifier(m_readFd, QSocketNotifier::Read, this);
    connect(m_readNotifier, &QSocketNotifier::activated, this, [this] {
        readDisplay();
    });
    const int waylandChild = wayland[1], wmChild = wm[1], notifyChild = notify[1];
    const std::vector<int> listenFds = m_listenFds;
    m_process.setChildProcessModifier([waylandChild, wmChild, notifyChild, listenFds] {
        if (close_range(3, ~0u, CLOSE_RANGE_CLOEXEC) < 0)
            _exit(127);
        sigset_t empty;
        sigemptyset(&empty);
        sigprocmask(SIG_SETMASK, &empty, nullptr);
        for (int signal : {SIGINT, SIGTERM, SIGPIPE, SIGHUP, SIGCHLD})
            ::signal(signal, SIG_DFL);
        fcntl(waylandChild, F_SETFD, 0);
        fcntl(wmChild, F_SETFD, 0);
        fcntl(notifyChild, F_SETFD, 0);
        for (int fd : listenFds)
            fcntl(fd, F_SETFD, 0);
    });
    auto env = childEnvironment();
    env.insert("WAYLAND_SOCKET", QString::number(waylandChild));
    m_process.setProcessEnvironment(env);
    m_process.setProcessChannelMode(QProcess::ForwardedChannels);
    QStringList args {"-rootless",  "-terminate",
                      "-nolisten",  "tcp",
                      "-auth",      m_authority,
                      "-displayfd", QString::number(notifyChild),
                      "-wm",        QString::number(wmChild)};
    // A restart takes the display the session already exported.
    if (!m_displayName.isEmpty())
        args << m_displayName;
    // The lock and the sockets are already ours.
    if (!m_listenFds.empty())
        args << "-nolock";
    for (int fd : m_listenFds)
        args << "-listenfd" << QString::number(fd);
    if (m_options.software)
        args << "-shm";
    m_process.start(m_options.executable, args);
    m_xwm->setServerProcess(m_process.processId());
    close(waylandChild);
    close(wmChild);
    close(notifyChild);
    m_xwm->start(wm[0]);
    m_timeout.start(m_options.timeoutMs);
    return true;
}

void XwaylandServer::readDisplay()
{
    char buffer[32];
    while (true) {
        const auto n = read(m_readFd, buffer, sizeof(buffer));
        if (n > 0) {
            m_readiness.append(buffer, n);
            if (m_readiness.size() > 12) {
                fail(QStringLiteral("oversized displayfd response"));
                return;
            }
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && errno == EAGAIN) {
            break;
        } else {
            if (!m_readiness.endsWith('\n')) {
                fail(QStringLiteral("displayfd closed before readiness"));
                return;
            }
            break;
        }
    }
    if (!m_readiness.contains('\n'))
        return;
    auto value = m_readiness;
    value.chop(1);
    bool valid = !value.isEmpty() && m_readiness.endsWith('\n');
    for (char c : value)
        valid &= c >= '0' && c <= '9';
    bool converted = false;
    const auto number = value.toUInt(&converted);
    if (!valid || !converted || number > 65535) {
        fail(QStringLiteral("malformed displayfd response"));
        return;
    }
    const QString name = ':' + QString::number(number);
    if (!m_displayName.isEmpty() && name != m_displayName) {
        fail(QStringLiteral("restarted on another display"));
        return;
    }
    m_displayName = name;
    m_readNotifier->setEnabled(false);
    close(std::exchange(m_readFd, -1));
    publish();
}

bool XwaylandServer::writeReadiness()
{
    if (!m_readinessFile.isEmpty()) {
        QSaveFile file(m_readinessFile);
        if (!file.open(QIODevice::WriteOnly) ||
            !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)) {
            fail(QStringLiteral("cannot create readiness file"));
            return false;
        }
        const auto json =
            QJsonDocument(QJsonObject {{"DISPLAY", m_displayName}, {"XAUTHORITY", m_authority}})
                .toJson(QJsonDocument::Compact);
        if (file.write(json) != json.size() || !file.commit()) {
            fail(QStringLiteral("cannot publish readiness file"));
            return false;
        }
    }
    return true;
}

void XwaylandServer::publish()
{
    if (m_ready || !m_xwmReady || !m_shell || !m_shell->isBound() || m_displayName.isEmpty() ||
        !m_error.isEmpty())
        return;
    // A lazy server published its display before Xwayland ran.
    if (!m_options.lazy && !writeReadiness())
        return;
    m_timeout.stop();
    m_ready = true;
    m_published = true;
    qInfo().noquote() << "Xwayland ready: DISPLAY=" + m_displayName << "XAUTHORITY=" + m_authority;
    if (!m_options.lazy && ready)
        ready();
    if (started)
        started();
}

void XwaylandServer::deferFailure(const QString& error)
{
    const auto epoch = m_epoch;
    QTimer::singleShot(0, this, [this, epoch, error] {
        if (epoch == m_epoch)
            fail(error);
    });
}

void XwaylandServer::fail(const QString& error)
{
    if (m_stopping || !m_error.isEmpty())
        return;
    // X11 clients die with the server, but new ones keep working: the session
    // exported DISPLAY and XAUTHORITY once, and a restart keeps both.
    if (m_published && mayRestart()) {
        qWarning() << "Xwayland lost:" << error << "- restarting";
        teardown(true);
        // A lazy server waits for the next client again.
        if (m_options.lazy)
            arm();
        else
            launch(); // A launch that fails reports through fail() itself.
        return;
    }
    m_error = error;
    qWarning() << "Xwayland unavailable:" << error;
    stop();
    if (failure)
        failure(error);
}

void XwaylandServer::setScale(double scale)
{
    if (scale <= 0 || scale == m_scale)
        return;
    m_scale = scale;
    applyScale();
}

void XwaylandServer::applyScale()
{
    if (m_client) {
        if (auto* server = wayland::Server::serverForDisplay(m_display))
            server->setClientScale(m_client, m_scale);
        // Existing surfaces take it at their next commit, which the X11
        // windows being resized below brings.
        wl_client_for_each_resource(
            m_client,
            [](wl_resource* resource, void* data) {
                if (std::strcmp(wl_resource_get_class(resource), "wl_surface") == 0) {
                    if (auto* surface = wayland::surfaceFromResource(resource))
                        surface->setClientScale(*static_cast<double*>(data));
                }
                return WL_ITERATOR_CONTINUE;
            },
            &m_scale);
    }
    if (m_xwm)
        m_xwm->setScale(m_scale);
}

bool XwaylandServer::mayRestart()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    while (!m_restarts.empty() && now - m_restarts.front() > 60000)
        m_restarts.pop_front();
    if (int(m_restarts.size()) >= m_options.restartLimit)
        return false;
    m_restarts.push_back(now);
    return true;
}

void XwaylandServer::stop()
{
    teardown(false);
    m_published = false;
}

void XwaylandServer::teardown(bool keepSession)
{
    if (m_stopping)
        return;
    m_stopping = true;
    ++m_epoch;
    m_timeout.stop();
    m_ready = m_xwmReady = false;
    delete std::exchange(m_readNotifier, nullptr);
    if (m_readFd >= 0)
        close(std::exchange(m_readFd, -1));
    m_xwm.reset();
    if (m_client)
        wl_client_destroy(m_client);
    m_shell.reset();
    m_keyboardGrab.reset();
    m_association.reset();
    if (m_process.state() != QProcess::NotRunning) {
        m_process.terminate();
        if (!m_process.waitForFinished(100)) {
            m_process.kill();
            m_process.waitForFinished(1000);
        }
    }
    m_readiness.clear();
    if (!keepSession) {
        closeSockets();
        if (!m_readinessFile.isEmpty())
            QFile::remove(m_readinessFile);
        m_runtime.reset();
        m_displayName.clear();
        m_authority.clear();
    }
    m_stopping = false;
}

} // namespace cutefish::xwayland
