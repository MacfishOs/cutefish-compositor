// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "scene/node.h"
#include "session/power_button.h"
#include "session/session_controller.h"
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusUnixFileDescriptor>
#include <QTest>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <unistd.h>

using namespace cutefish;

class FakeLogind : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.login1.Manager")
public:
    QStringList lastInhibit;

public slots:
    QDBusUnixFileDescriptor Inhibit(const QString &what, const QString &who, const QString &why,
                                    const QString &mode)
    {
        lastInhibit = {what, who, why, mode};
        int ends[2];
        if (pipe2(ends, O_CLOEXEC) != 0)
            return {};
        close(ends[0]);
        const QDBusUnixFileDescriptor descriptor(ends[1]);
        close(ends[1]);
        return descriptor;
    }
};

struct Keys : input::EventSink {
    QList<uint32_t> keys;

    bool onKey(input::Timestamp, uint32_t evdevCode, bool pressed) override
    {
        if (pressed)
            keys.append(evdevCode);
        return false;
    }
};

class PowerButtonTest : public QObject
{
    Q_OBJECT
private:
    QDBusConnection logindBus = QDBusConnection::connectToBus(QDBusConnection::SessionBus,
                                                              QStringLiteral("fake-logind"));
    FakeLogind logind;

    struct Fixture {
        scene::Node root;
        session::SecureContainer secure {&root};
        session::SessionController controller {&secure};
        Keys next;
        session::PowerButton button {&controller, &next, QDBusConnection::sessionBus()};

        Fixture() { controller.setOutputs({{QStringLiteral("one"), QRect(0, 0, 800, 600)}}); }

        void unlock()
        {
            controller.notifyOutputPresented(QStringLiteral("one"));
            QVERIFY(controller.requestUnlock());
            controller.notifyOutputPresented(QStringLiteral("one"));
            QCOMPARE(controller.state(), session::SessionState::Active);
        }

        void tap()
        {
            QVERIFY(button.onKey(input::Timestamp {}, KEY_POWER, true));
            QVERIFY(button.onKey(input::Timestamp {}, KEY_POWER, false));
        }
    };

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

    void aTapLocks()
    {
        Fixture fixture;
        QVERIFY(fixture.button.start());
        QTRY_VERIFY(fixture.button.holdsInhibitor());
        QCOMPARE(logind.lastInhibit.value(0), QStringLiteral("handle-power-key"));
        QCOMPARE(logind.lastInhibit.value(3), QStringLiteral("block"));

        fixture.tap();
        QCOMPARE(fixture.controller.state(), session::SessionState::Locking);
        QVERIFY(fixture.next.keys.isEmpty());
    }

    void theKeyPassesOnUntilLogindLetsGo()
    {
        Fixture fixture;
        fixture.button.onKey(input::Timestamp {}, KEY_POWER, true);
        fixture.button.onKey(input::Timestamp {}, KEY_POWER, false);
        fixture.button.onKey(input::Timestamp {}, KEY_A, true);
        QCOMPARE(fixture.next.keys, (QList<uint32_t> {KEY_POWER, KEY_A}));
        QCOMPARE(fixture.controller.state(), session::SessionState::Active);
    }

    void thePressThatWokeTheSystemDoesNotLock()
    {
        Fixture fixture;
        QVERIFY(fixture.button.start());
        QTRY_VERIFY(fixture.button.holdsInhibitor());
        prepareForSleep(true);
        prepareForSleep(false);
        QTest::qWait(100);
        fixture.tap();
        QCOMPARE(fixture.controller.state(), session::SessionState::Active);
    }

    void rapidTapsActOnce()
    {
        Fixture fixture;
        QVERIFY(fixture.button.start());
        QTRY_VERIFY(fixture.button.holdsInhibitor());
        fixture.tap();
        fixture.unlock();
        fixture.tap();
        QCOMPARE(fixture.controller.state(), session::SessionState::Active);
        QTest::qWait(600);
        fixture.tap();
        QCOMPARE(fixture.controller.state(), session::SessionState::Locking);
    }
};

QTEST_GUILESS_MAIN(PowerButtonTest)

#include "power_button_test.moc"
