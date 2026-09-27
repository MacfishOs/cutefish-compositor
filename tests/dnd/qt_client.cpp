// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstdio>

#include <QApplication>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QTimer>
#include <QWidget>

class Window : public QWidget
{
public:
    explicit Window(bool source)
        : m_source(source)
    {
        setAcceptDrops(!source);
        resize(180, 120);
    }

protected:
    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!m_source || !(event->buttons() & Qt::LeftButton) || m_started)
            return;
        m_started = true;
        // On the heap, as Qt requires: the xcb backend serves the data after
        // exec() returns, from the drag it keeps for the drop.
        auto* drag = new QDrag(this);
        auto* mime = new QMimeData;
        mime->setData("application/x-cutefish-dnd", QByteArray(131072, 'd'));
        mime->setText(QStringLiteral("Cutefish drag"));
        drag->setMimeData(mime);
        QPixmap icon(32, 32);
        icon.fill(Qt::blue);
        drag->setPixmap(icon);
        drag->setHotSpot(QPoint(7, 9));
        std::puts("STARTED");
        std::fflush(stdout);
        const auto result = drag->exec(Qt::CopyAction | Qt::MoveAction, Qt::MoveAction);
        std::puts(result == Qt::IgnoreAction ? "CANCELLED" : "FINISHED");
        std::fflush(stdout);
    }

    void dragEnterEvent(QDragEnterEvent* event) override
    {
        if (event->mimeData()->hasFormat("application/x-cutefish-dnd"))
            event->acceptProposedAction();
    }

    void dropEvent(QDropEvent* event) override
    {
        const QByteArray data = event->mimeData()->data("application/x-cutefish-dnd");
        const bool valid = data == QByteArray(131072, 'd');
        if (valid)
            event->acceptProposedAction();
        else
            std::fprintf(stderr, "received %lld bytes\n", static_cast<long long>(data.size()));
        std::puts(valid ? "PASS" : "FAIL");
        std::fflush(stdout);
    }

private:
    bool m_source;
    bool m_started = false;
};

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    const QString title = app.arguments().value(1);
    Window window(title.endsWith("source"));
    window.setWindowTitle(title);
    window.show();
    QTimer::singleShot(15000, &app, &QCoreApplication::quit);
    return app.exec();
}
