// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "scene/node.h"
#include "session/session_controller.h"
#include "session/sleep_lock.h"
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusUnixFileDescriptor>
#include <QTest>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

using namespace cutefish;

// logind's manager, as much of it as the lock uses: Inhibit hands out the
// write end of a pipe, and the read end tells whether anyone still holds it.
class FakeLogind : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.login1.Manager")
public:
    ~FakeLogind() override
    {
        for (int fd : readEnds)
            close(fd);
    }

    QStringList lastInhibit;
    std::vector<int> readEnds;

    // Whether the descriptor handed out last is still open anywhere.
    bool held() const
    {
        if (readEnds.empty())
            return false;
        pollfd descriptor {readEnds.back(), POLLIN, 0};
        return poll(&descriptor, 1, 0) == 0;
    }

public slots:
    QDBusUnixFileDescriptor Inhibit(const QString &what, const QString &who, const QString &why,
                                    const QString &mode)
    {
        lastInhibit = {what, who, why, mode};
        int ends[2];
        if (pipe2(ends, O_CLOEXEC) != 0)
            return {};
        readEnds.push_back(ends[0]);
        const QDBusUnixFileDescriptor descriptor(ends[1]);
        close(ends[1]);
        return descriptor;
    }
};

class SleepLockTest : public QObject
{
    Q_OBJECT
private:
    QDBusConnection logindBus = QDBusConnection::connectToBus(QDBusConnection::SessionBus,
                                                              QStringLiteral("fake-logind"));
    FakeLogind logind;

    void prepareForSleep(bool sleeping)
    {
        auto signal = QDBusMessage::createSignal(QStringLiteral("/org/freedesktop/login1"),
                                                 QStringLiteral("org.freedesktop.login1.Manager"),
                                                 QStringLiteral("PrepareForSleep"));
        signal << sleeping;
        QVERIFY(logindBus.send(signal));
    }

private slots:
    void initTestCase()
    {
        QVERIFY(logindBus.isConnected());
        QVERIFY(logindBus.registerObject(QStringLiteral("/org/freedesktop/login1"), &logind,
                                         QDBusConnection::ExportAllSlots));
        QVERIFY(logindBus.registerService(QStringLiteral("org.freedesktop.login1")));
    }

    void sleepWaitsForTheLockOnEveryOutput()
    {
        scene::Node root;
        session::SecureContainer secure(&root);
        session::SessionController controller(&secure);
        controller.setOutputs({{QStringLiteral("one"), QRect(0, 0, 800, 600)},
                               {QStringLiteral("two"), QRect(800, 0, 800, 600)}});
        session::SleepLock lock(&controller, QDBusConnection::sessionBus());
        QVERIFY(lock.start());
        QTRY_VERIFY(lock.holdsInhibitor());
        QCOMPARE(logind.lastInhibit.value(0), QStringLiteral("sleep"));
        QCOMPARE(logind.lastInhibit.value(3), QStringLiteral("delay"));
        QVERIFY(logind.held());

        prepareForSleep(true);
        QTRY_COMPARE(controller.state(), session::SessionState::Locking);
        controller.notifyOutputPresented(QStringLiteral("one"));
        QCoreApplication::processEvents();
        QVERIFY(lock.holdsInhibitor());
        QVERIFY(logind.held());
        controller.notifyOutputPresented(QStringLiteral("two"));
        QCOMPARE(controller.state(), session::SessionState::Locked);
        QVERIFY(!lock.holdsInhibitor());
        QTRY_VERIFY(!logind.held());

        // Awake again: the next sleep is held back too.
        const size_t inhibits = logind.readEnds.size();
        prepareForSleep(false);
        QTRY_VERIFY(lock.holdsInhibitor());
        QCOMPARE(logind.readEnds.size(), inhibits + 1);
        QVERIFY(logind.held());
    }

    void anUnlockInProgressIsLockedAgainBeforeSleeping()
    {
        scene::Node root;
        session::SecureContainer secure(&root);
        session::SessionController controller(&secure);
        controller.setOutputs({{QStringLiteral("one"), QRect(0, 0, 800, 600)}});
        session::SleepLock lock(&controller, QDBusConnection::sessionBus());
        QVERIFY(lock.start());
        QTRY_VERIFY(lock.holdsInhibitor());
        QVERIFY(controller.requestLock());
        controller.notifyOutputPresented(QStringLiteral("one"));
        QVERIFY(controller.requestUnlock());
        QCOMPARE(controller.state(), session::SessionState::Unlocking);

        prepareForSleep(true);
        QTest::qWait(50);
        QCOMPARE(controller.state(), session::SessionState::Unlocking);
        QVERIFY(lock.holdsInhibitor());
        controller.notifyOutputPresented(QStringLiteral("one"));
        QTRY_COMPARE(controller.state(), session::SessionState::Locking);
        QVERIFY(lock.holdsInhibitor());
        controller.notifyOutputPresented(QStringLiteral("one"));
        QCOMPARE(controller.state(), session::SessionState::Locked);
        QTRY_VERIFY(!logind.held());
        prepareForSleep(false);
        QTRY_VERIFY(lock.holdsInhibitor());
    }

    void anAlreadyLockedSessionSleepsAtOnce()
    {
        scene::Node root;
        session::SecureContainer secure(&root);
        session::SessionController controller(&secure);
        controller.setOutputs({{QStringLiteral("one"), QRect(0, 0, 800, 600)}});
        session::SleepLock lock(&controller, QDBusConnection::sessionBus());
        QVERIFY(lock.start());
        QTRY_VERIFY(lock.holdsInhibitor());
        QVERIFY(controller.requestLock());
        controller.notifyOutputPresented(QStringLiteral("one"));
        QCOMPARE(controller.state(), session::SessionState::Locked);

        prepareForSleep(true);
        QTRY_VERIFY(!logind.held());
        QVERIFY(!lock.holdsInhibitor());
        prepareForSleep(false);
        QTRY_VERIFY(lock.holdsInhibitor());
    }
};

QTEST_GUILESS_MAIN(SleepLockTest)
#include "sleep_lock_test.moc"
