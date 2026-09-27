// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstdio>

#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QTimer>
#include <QWidget>
#include <QWindow>

namespace {

const QString text = QString::fromUtf8("Generated clipboard 🐟 中文\nsecond line");
const QString mime = QStringLiteral("application/x-cutefish-toolkit-test");

QByteArray binary()
{
    QByteArray data(2097169, Qt::Uninitialized);
    for (qsizetype i = 0; i < data.size(); ++i)
        data[i] = static_cast<char>(i % 256);
    return data;
}

class Window : public QWidget
{
public:
    bool primary = false;
    bool raw = false;
    bool urls = false;

    void changeEvent(QEvent *event) override
    {
        QWidget::changeEvent(event);
        if (event->type() == QEvent::ActivationChange && isActiveWindow()) {
            std::puts("FOCUSED");
            std::fflush(stdout);
        }
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Q) {
            QApplication::quit();
        } else if (event->key() == Qt::Key_D) {
            auto *dialog = new QDialog(this);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->setWindowTitle("clipboard-dialog");
            dialog->resize(200, 100);
            dialog->setModal(true);
            dialog->show();
        } else if (event->key() == Qt::Key_M) {
            auto *menu = new QMenu(this);
            menu->addAction("Copy", this, [this] {
                QKeyEvent copy(QEvent::KeyPress, Qt::Key_C, Qt::NoModifier);
                keyPressEvent(&copy);
            });
            QObject::connect(menu, &QMenu::aboutToHide, menu, [] {
                QTimer::singleShot(0, [] {
                    QGuiApplication::sync();
                    std::puts("MENU_CLOSED");
                    std::fflush(stdout);
                });
            });
            menu->popup(mapToGlobal(QPoint(20, 20)));
            std::puts("MENU");
            std::fflush(stdout);
        } else if (event->key() == Qt::Key_C) {
            if (urls) {
                auto *data = new QMimeData;
                data->setUrls({QUrl("file:///tmp/clipboard-test/item.txt")});
                QApplication::clipboard()->setMimeData(data);
            } else if (raw) {
                auto *data = new QMimeData;
                data->setData(mime, binary());
                QApplication::clipboard()->setMimeData(data);
            } else {
                QApplication::clipboard()->setText(
                        text, primary ? QClipboard::Selection : QClipboard::Clipboard);
            }
            std::puts("PUBLISHED");
            std::fflush(stdout);
        } else if (event->key() == Qt::Key_V || event->key() == Qt::Key_P) {
            auto *clipboard = QApplication::clipboard();
            const bool ok = urls ? clipboard->mimeData()->urls()
                            == QList<QUrl> {QUrl("file:///tmp/clipboard-test/item.txt")}
                    : raw ? clipboard->mimeData()->data(mime) == binary()
                          : clipboard->text(primary ? QClipboard::Selection : QClipboard::Clipboard)
                            == text;
            std::puts(ok ? "PASS" : "FAIL");
            std::fflush(stdout);
            if (event->key() == Qt::Key_V || !ok)
                QApplication::exit(ok ? 0 : 2);
        }
    }
};

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QObject::connect(&app, &QGuiApplication::focusWindowChanged, &app, [](QWindow *window) {
        if (window) {
            std::printf("ACTIVE %s\n", window->title().toUtf8().constData());
            std::fflush(stdout);
        }
    });
    Window window;
    window.primary = app.arguments().contains(QStringLiteral("primary"));
    window.raw = app.arguments().contains(QStringLiteral("binary"));
    window.urls = app.arguments().contains(QStringLiteral("urls"));
    if (window.urls) {
        QObject::connect(QApplication::clipboard(), &QClipboard::dataChanged, &window, [] {
            const auto *data = QApplication::clipboard()->mimeData();
            if (data)
                data->urls();
        });
    }
    window.setWindowTitle(QString::fromLocal8Bit(argv[1]));
    window.resize(320, 200);
    window.show();
    QTimer::singleShot(15000, &app, [] {
        QApplication::exit(3);
    });
    return app.exec();
}
