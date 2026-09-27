// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QTimer>

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty("damageScroll",
                                             qEnvironmentVariableIsSet("CUTEFISH_TEST_SCROLL"));
    QQmlComponent component(&engine);
    component.setData(R"(import QtQuick
import FishUI 1.0 as FishUI
FishUI.Window {
    width: 640; height: 480
    title: "fishui-resize-test"
    Rectangle {
        anchors.fill: parent
        color: "white"
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

        Text { anchors.centerIn: parent; text: parent.width + " x " + parent.height; color: "black" }
    }
})",
                      QUrl());
    std::unique_ptr<QObject> window(component.create());
    if (!window) {
        qWarning() << component.errors();
        return 1;
    }
    QTimer::singleShot(15000, &app, &QCoreApplication::quit);
    return app.exec();
}
