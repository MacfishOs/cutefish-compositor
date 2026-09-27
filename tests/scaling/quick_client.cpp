// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstdio>

#include <QGuiApplication>
#include <QMouseEvent>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QScreen>
#include <QTimer>

class Window : public QQuickView
{
public:
    Window()
    {
        setTitle(QStringLiteral("fractional-scale-quick"));
        setResizeMode(SizeRootObjectToView);
        engine()->rootContext()->setContextProperty(
                "damageScroll", qEnvironmentVariableIsSet("CUTEFISH_TEST_SCROLL"));
        QQmlComponent component(engine());
        component.setData(R"(import QtQuick
Rectangle {
    width: 801; height: 601; color: "white"
    Text { x: 20; y: 20; text: "Native density 中文 🐟"; color: "black" }
    Flickable {
        id: scrolling
        visible: damageScroll
        x: 10; y: 60; width: parent.width - 20; height: parent.height - 70
        contentHeight: 2000; clip: true
        Repeater {
            model: 50
            Rectangle { y: index * 40; width: scrolling.width; height: 20; color: "#2468ac" }
        }
        Timer {
            interval: 50; running: damageScroll; repeat: true
            onTriggered: scrolling.contentY = (scrolling.contentY + 9) % 1000
        }
    }

})",
                          QUrl());
        setContent(QUrl(), &component, component.create());
        connect(this, &QQuickWindow::frameSwapped, this, [this] {
            report();
        });
    }

    void report()
    {
        const QRect geometry = screen()->geometry();
        std::printf("METRICS Qt=%s screen=%dx%d screenDpr=%.2f window=%dx%d windowDpr=%.2f\n",
                    qVersion(), geometry.width(), geometry.height(), screen()->devicePixelRatio(),
                    width(), height(), devicePixelRatio());
        std::fflush(stdout);
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::Enter) {
            auto *enter = static_cast<QEnterEvent *>(event);
            std::printf("ENTER %.2f %.2f\n", enter->position().x(), enter->position().y());
            std::fflush(stdout);
        }
        return QQuickView::event(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        std::printf("POINTER %.2f %.2f\n", event->position().x(), event->position().y());
        std::fflush(stdout);
        QQuickView::mouseMoveEvent(event);
    }
};

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    Window window;
    window.show();
    QTimer::singleShot(15000, &app, [] {
        QCoreApplication::exit(2);
    });
    return app.exec();
}
