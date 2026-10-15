// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QThread>
#include <QTest>

#include "compositor/compositor.h"
#include "compositor/surface.h"
#include "fake_renderer.h"
#include "input/seat.h"
#include "scene/node.h"
#include "scene/scene.h"
#include "shm_test_util.h"
#include "decoration/decoration_theme.h"
#include "decoration/shadow_node.h"
#include "switcher/application_info.h"
#include "switcher/switcher_view.h"
#include "wm/window.h"
#include "wm/window_manager.h"

using namespace cutefish;

class SwitcherViewTest : public QObject
{
    Q_OBJECT

private:
    struct Desktop {
        scene::Scene scene;
        scene::Node *surfaces =
                scene.root()->addChild(std::make_unique<scene::Node>(QStringLiteral("surfaces")));
        scene::Node *switcher =
                scene.root()->addChild(std::make_unique<scene::Node>(QStringLiteral("switcher")));
        compositor::Compositor compositor {surfaces};
        testsupport::FakeRenderer renderer;
        input::Seat seat {&compositor};
        wm::WindowManager manager {&compositor, &seat};
        std::vector<std::unique_ptr<compositor::Surface>> owned;
        std::shared_ptr<compositor::ShmPool> pool = testsupport::makePool(1 << 20);

        Desktop() { manager.setOutputBounds(QRect(0, 0, 1000, 800)); }
        ~Desktop() { seat.shutdown(); }

        wm::Window *open(const QString &applicationId, const QString &title)
        {
            owned.push_back(compositor.createSurface());
            auto *surface = owned.back().get();
            surface->attach(pool->createBuffer({200, 100}, 0, 800, render::PixelFormat::Bgrx8888,
                                               nullptr),
                            {});
            surface->commit();
            compositor.updateTextures(renderer);
            return manager.createWindow(surface, {.requestedSize = {200, 100},
                                                  .title = title,
                                                  .applicationId = applicationId});
        }
    };

    static void writeEntry(const QString &path, const QByteArray &body)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("[Desktop Entry]\nType=Application\n" + body);
    }

private slots:
    void findsEntriesByIdAndWindowClassAndRemembersMisses()
    {
        const QString applications = qEnvironmentVariable("XDG_DATA_HOME") + "/applications";
        QVERIFY(QDir().mkpath(applications));
        writeEntry(applications + "/org.example.Editor.desktop",
                   "Name=Editor\nIcon=editor-icon\n");
        writeEntry(applications + "/viewer.desktop",
                   "Name=Viewer\nIcon=viewer-icon\nStartupWMClass=ViewerWindow\n");

        switcher::ApplicationInfoCache cache;
        QCOMPARE(cache.lookup("org.example.Editor", "x").name, QStringLiteral("Editor"));
        QCOMPARE(cache.lookup("org.example.Editor", "x").icon, QStringLiteral("editor-icon"));
        QCOMPARE(cache.lookup("viewerwindow", "x").name, QStringLiteral("Viewer"));
        QCOMPARE(cache.lookup("paint", "x").name, QStringLiteral("paint"));
        QCOMPARE(cache.lookup("", "Untitled").name, QStringLiteral("Untitled"));

        // An application installed since is found.
        QThread::msleep(20);
        writeEntry(applications + "/paint.desktop", "Name=Paint\nIcon=paint-icon\n");
        cache.refresh();
        QCOMPARE(cache.lookup("paint", "x").name, QStringLiteral("Paint"));
    }

    void drawsARowCentredOnTheOutputAndFindsItsEntries()
    {
        Desktop desktop;
        wm::Window *editor = desktop.open(QStringLiteral("no-such-editor"), QStringLiteral("Doc"));
        wm::Window *viewer = desktop.open(QString(), QStringLiteral("Viewer"));
        switcher::SwitcherView view(desktop.switcher, &desktop.manager, nullptr);

        view.showWindowCycle({editor, viewer}, 1, QString());
        const QRect bounds = view.bounds();
        const qreal cell = switcher::SwitcherView::kCellSize;
        QCOMPARE(bounds.size(),
                 QSize(qRound(2 * cell + 2 * switcher::SwitcherView::kPadding),
                       qRound(cell + 2 * switcher::SwitcherView::kVerticalPadding)));
        QCOMPARE(bounds.center(), QRect(0, 0, 1000, 800).center());

        const QImage image = view.renderImage();
        QVERIFY(!image.isNull());
        QCOMPARE(image.size(), bounds.size());
        const int padding = qRound(switcher::SwitcherView::kPadding);
        const int top = qRound(switcher::SwitcherView::kVerticalPadding);
        // The selection's glass sits centred on the second cell.
        const QRect selection = view.selectionBounds();
        const QPointF secondCell = QPointF(bounds.topLeft() + QPoint(padding, top))
                + QPointF(cell * 1.5, cell / 2);
        QCOMPARE(QRectF(selection).center(), secondCell);
        QCOMPARE(selection.width(), qRound(cell * switcher::SwitcherView::kSelectionRatio));

        // Moving the selection repaints only what changed; the rest stays.
        const QPoint firstIcon(padding + qRound(cell / 2), top + qRound(cell / 2));
        QVERIFY(qAlpha(image.pixel(firstIcon)) > 0);
        view.selectWindowCycleEntry(0);
        const QImage moved = view.renderImage();
        QVERIFY(qAlpha(moved.pixel(firstIcon + QPoint(qRound(cell), 0))) > 0);
        QVERIFY(qAlpha(moved.pixel(firstIcon)) > 0);
        QCOMPARE(view.selectionBounds().translated(qRound(cell), 0), selection);

        const QPointF row = bounds.topLeft() + QPointF(padding, top);
        QCOMPARE(view.windowCycleEntryAt(row + QPointF(10, 10)), 0);
        QCOMPARE(view.windowCycleEntryAt(row + QPointF(cell + 10, 10)), 1);
        QCOMPARE(view.windowCycleEntryAt(row + QPointF(10, cell + 10)), -1);
        QCOMPARE(view.windowCycleEntryAt(bounds.topLeft() - QPointF(5, 5)), -1);
        QVERIFY(view.windowCycleViewContains(bounds.center()));

        view.hideWindowCycle();
        QVERIFY(!view.windowCycleViewContains(bounds.center()));
        QVERIFY(view.renderImage().isNull());
    }

    void aListOfAnotherLengthGetsATextureOfItsSize()
    {
        Desktop desktop;
        wm::Window *first = desktop.open(QStringLiteral("one"), QString());
        wm::Window *second = desktop.open(QStringLiteral("two"), QString());
        wm::Window *third = desktop.open(QStringLiteral("three"), QString());
        switcher::SwitcherView view(desktop.switcher, &desktop.manager, nullptr);
        const auto drawnSize = [&desktop] {
            desktop.renderer.calls.clear();
            desktop.switcher->render(desktop.renderer);
            for (const auto &call : desktop.renderer.calls) {
                if (call.kind == QLatin1String("texture"))
                    return call.texture->size();
            }
            return QSize();
        };

        view.showWindowCycle({first}, 0, QString());
        QCOMPARE(drawnSize(), view.renderImage().size());
        view.hideWindowCycle();
        view.showWindowCycle({first, second, third}, 1, QString());
        QCOMPARE(drawnSize(), view.renderImage().size());
    }

    void thePanelCastsAShadow()
    {
        Desktop desktop;
        wm::Window *window = desktop.open(QStringLiteral("one"), QString());
        decoration::Theme theme;
        switcher::SwitcherView view(desktop.switcher, &desktop.manager, &theme);
        view.showWindowCycle({window}, 0, QString());
        auto *shadow = dynamic_cast<decoration::ShadowNode *>(desktop.switcher->children().front());
        QVERIFY(shadow);
        QVERIFY(shadow->isVisible());
        QVERIFY(shadow->elevation() != wm::ShadowElevation::None);
        QVERIFY(shadow->absoluteGeometry().contains(view.bounds()));
        // The glass's own corner: a tighter one shows past it as a dark crescent.
        QCOMPARE(shadow->radius(), switcher::SwitcherView::kRadius);
    }

    void shrinksTheCellsToFitTheOutput()
    {
        Desktop desktop;
        std::vector<wm::Window *> windows;
        for (int i = 0; i < 12; ++i)
            windows.push_back(desktop.open(QStringLiteral("app%1").arg(i), QString()));
        switcher::SwitcherView view(desktop.switcher, &desktop.manager, nullptr);
        view.showWindowCycle(windows, 0, QString());
        QVERIFY(view.bounds().width() <= 1000 - 2 * switcher::SwitcherView::kScreenMargin);
        QVERIFY(view.bounds().left() >= 0);
    }
};

int main(int argc, char **argv)
{
    // The test's desktop entries come first; the system's icon themes stay.
    QTemporaryDir data;
    qputenv("XDG_DATA_HOME", data.path().toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    SwitcherViewTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "switcher_view_test.moc"
