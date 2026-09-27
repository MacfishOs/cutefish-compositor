// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstdio>

#include <QApplication>
#include <QEnterEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QThread>
#include <QTimer>
#include <QWidget>
#include <QWindow>

class Window : public QWidget
{
    int m_scroll = 0;

public:
    Window()
    {
        setMouseTracking(true);
        setWindowTitle(QStringLiteral("fractional-scale-qt"));
        resize(801, 601);
        if (qEnvironmentVariableIsSet("CUTEFISH_TEST_SCROLL")) {
            auto *timer = new QTimer(this);
            connect(timer, &QTimer::timeout, this, [this] {
                m_scroll = (m_scroll + 9) % 40;
                update(QRect(10, 50, width() - 20, height() - 60));
            });
            timer->start(50);
        }
    }

    void paintEvent(QPaintEvent *) override
    {
        if (qEnvironmentVariableIsSet("CUTEFISH_TEST_SLOW_PAINT"))
            QThread::msleep(75);
        QPainter painter(this);
        painter.fillRect(rect(), Qt::white);
        painter.setPen(Qt::black);
        painter.drawText(20, 30, QStringLiteral("Native density 中文 🐟"));
        if (qEnvironmentVariableIsSet("CUTEFISH_TEST_SCROLL")) {
            painter.setClipRect(QRect(10, 50, width() - 20, height() - 60));
            for (int y = 50 - m_scroll; y < height(); y += 40)
                painter.fillRect(10, y, width() - 20, 20, QColor("#2468ac"));
        }
        report();
    }

    void report()
    {
        const QRect screenGeometry = screen()->geometry();
        std::printf("METRICS Qt=%s screen=%dx%d screenDpr=%.2f window=%dx%d windowDpr=%.2f\n",
                    qVersion(), screenGeometry.width(), screenGeometry.height(),
                    screen()->devicePixelRatio(), width(), height(),
                    windowHandle()->devicePixelRatio());
        std::fflush(stdout);
    }

    void enterEvent(QEnterEvent *event) override
    {
        std::printf("ENTER %.2f %.2f\n", event->position().x(), event->position().y());
        std::fflush(stdout);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        std::printf("POINTER %.2f %.2f\n", event->position().x(), event->position().y());
        std::fflush(stdout);
    }
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    Window window;
    window.show();
    QObject::connect(window.screen(), &QScreen::geometryChanged, &window, [&] {
        window.report();
    });
    QTimer::singleShot(15000, &app, [] {
        QCoreApplication::exit(2);
    });
    return app.exec();
}
