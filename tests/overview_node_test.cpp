// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The overview node's progress - the tween, the fingers, the rubber band and
// the hand-offs between them - on a fake frame clock, and where it draws the
// windows it moves.

#include <algorithm>
#include <cmath>

#include <gtest/gtest.h>

#include "fake_renderer.h"
#include "scene/overview_node.h"
#include "scene/scene.h"
#include "scene/window_animation.h"

using namespace cutefish;
using Node = scene::OverviewNode;
using Track = scene::motion::FingerTrack;

namespace {

class FrameTexture : public render::Texture
{
public:
    explicit FrameTexture(QSize size)
        : m_size(size)
    {
    }

    QSize size() const override { return m_size; }

private:
    QSize m_size;
};

class FrameRenderer : public testsupport::FakeRenderer
{
public:
    bool renderToTexture(std::unique_ptr<render::Texture> &texture, const QRect &bounds,
                         const std::function<void()> &paint) override
    {
        paint();
        if (!texture || texture->size() != bounds.size())
            texture = std::make_unique<FrameTexture>(bounds.size());
        return true;
    }

    void drawTextureF(render::Texture *texture, const QRectF &target, float alpha) override
    {
        FakeRenderer::drawTextureF(texture, target, alpha);
        calls.last().target = target;
    }

    QList<render::Texture *> trilinear;

    void enableTrilinearFiltering(render::Texture *texture) override { trilinear.append(texture); }
};

constexpr QRect kViewport(0, 0, 1000, 800);
constexpr QRect kWindow(100, 100, 400, 300);
constexpr QRectF kPlace(600, 400, 200, 150);
constexpr int kSettleMs = 1500;

struct Fixture {
    qint64 now = 1000;
    scene::Scene scene;
    scene::Node *container =
            scene.root()->addChild(std::make_unique<scene::Node>(QStringLiteral("surfaces")));
    scene::ColorNode *desktop = static_cast<scene::ColorNode *>(
            container->addChild(std::make_unique<scene::ColorNode>(render::Color {0, 0, 1, 1})));
    scene::WindowAnimation *window = static_cast<scene::WindowAnimation *>(
            container->addChild(std::make_unique<scene::WindowAnimation>([this] {
                return double(now);
            })));
    Node *node = static_cast<Node *>(container->addChild(std::make_unique<Node>([this] {
        return now;
    })));
    FrameRenderer renderer;
    QList<double> finished;

    Fixture()
    {
        scene.setSize(kViewport.size());
        desktop->setGeometry(kViewport);
        window->setGeometry(kWindow);
        window->addChild(std::make_unique<scene::ColorNode>(render::Color {1, 0, 0, 1}))
                ->setGeometry(QRect(QPoint(), kWindow.size()));
        // Already open: no animation of its own.
        window->finish();
        node->setGeometry(kViewport);
        node->setBackdrop({desktop});
        node->setItems({item()}, false);
        node->setFinishedCallback([this](double target) {
            finished.append(target);
        });
    }

    Node::Item item(const QRectF &to = kPlace) const
    {
        Node::Item item;
        item.windows = {window};
        item.to = to;
        item.title = QStringLiteral("Window");
        return item;
    }

    void paint()
    {
        renderer.calls.clear();
        renderer.setOutputContext({QStringLiteral("out"), kViewport, kViewport.size(), 1, 0, 0, {}});
        renderer.frameTime = double(now);
        container->render(renderer);
    }

    void settle()
    {
        paint();
        now += kSettleMs;
        paint();
    }

    QRectF rect(double progress) const
    {
        return node->presentedRect(0, progress, double(now));
    }
};

QRectF lerp(const QRectF &a, const QRectF &b, double t)
{
    return QRectF(a.x() + (b.x() - a.x()) * t, a.y() + (b.y() - a.y()) * t,
                  a.width() + (b.width() - a.width()) * t,
                  a.height() + (b.height() - a.height()) * t);
}

} // namespace

TEST(OverviewNode, AtZeroItCoversNothingAndDrawsNothing)
{
    Fixture f;
    EXPECT_FALSE(f.node->isActive());
    EXPECT_EQ(scene::Node::Content::None, f.node->content());
    EXPECT_FALSE(f.node->occludesOutput(f.renderer));
    f.paint();
    // The desk as it is: the desktop and the window, each drawn once.
    const QStringList kinds = f.renderer.kinds();
    EXPECT_EQ(2, kinds.count(QStringLiteral("fill")));
}

TEST(OverviewNode, TheShortcutGlidesInAndLandsOnTime)
{
    Fixture f;
    const qint64 start = f.now;
    f.node->animateTo(1);
    EXPECT_TRUE(f.node->isActive());
    EXPECT_TRUE(f.node->occludesOutput(f.renderer));
    double last = 0;
    while (f.now - start < qint64(Node::kRestDurationMs)) {
        f.now += 4;
        f.paint();
        const double progress = f.node->progress(double(f.now));
        EXPECT_GE(progress, last);
        EXPECT_LE(progress, 1);
        last = progress;
        // From rest it sets off gently: a tenth of the time covers under 3%.
        if (f.now - start == qint64(Node::kRestDurationMs / 10))
            EXPECT_LT(progress, 0.03);
    }
    // No tail creeping in after its time.
    EXPECT_EQ(1, last);
    f.paint();
    EXPECT_EQ(QList<double>({1}), f.finished);
    EXPECT_TRUE(f.node->isSettled());
    EXPECT_EQ(1, f.node->target());

    // And back, to the desk as it was, where it covers nothing again.
    f.node->animateTo(0);
    f.settle();
    EXPECT_EQ(QList<double>({1, 0}), f.finished);
    EXPECT_FALSE(f.node->isActive());
}

TEST(OverviewNode, AWindowMovesFromWhereItIsToItsPlace)
{
    Fixture f;
    EXPECT_EQ(QRectF(kWindow), f.rect(0));
    EXPECT_EQ(kPlace, f.rect(1));
    EXPECT_EQ(lerp(kWindow, kPlace, 0.5), f.rect(0.5));
}

TEST(OverviewNode, TheWindowIsDrawnScaledOntoItsPlace)
{
    Fixture f;
    f.node->jumpTo(1);
    f.paint();
    // The window's texture holds its shadow margins, scaled along with it.
    const double scale = kPlace.width() / kWindow.width();
    const QRectF coverage = QRectF(kWindow).adjusted(-64, -64, 64, 64);
    const QRectF expected(kPlace.topLeft() + (coverage.topLeft() - kWindow.topLeft()) * scale,
                          coverage.size() * scale);
    const auto drawn = std::find_if(f.renderer.calls.begin(), f.renderer.calls.end(),
                                    [](const auto &call) {
                                        return call.kind == QStringLiteral("texture");
                                    });
    ASSERT_NE(f.renderer.calls.end(), drawn);
    EXPECT_EQ(expected, drawn->target);
    // Half its size, it is sampled through mipmaps.
    EXPECT_EQ(1, f.renderer.trilinear.size());
    // The desktop under it, dimmed.
    ASSERT_GE(f.renderer.calls.size(), 3);
    EXPECT_EQ(QStringLiteral("fill"), f.renderer.calls[0].kind);
    EXPECT_FLOAT_EQ(1, f.renderer.calls[1].color.b);
    EXPECT_FLOAT_EQ(Node::kDimOpacity, f.renderer.calls[2].color.a);
}

TEST(OverviewNode, TheFingersMoveItAndPullAgainstTheEnds)
{
    Fixture f;
    f.node->startTracking();
    for (int step = 1; step <= 5; ++step) {
        f.now += 10;
        f.node->updateTracking(0.1, double(f.now));
    }
    // Shown where the fingers were a resampling latency ago.
    const double shown = f.node->progress(double(f.now) + Track::kResampleLatencyMs);
    EXPECT_NEAR(0.5, shown, 1e-9);

    // Far past the end, it gives only so much.
    for (int step = 0; step < 20; ++step) {
        f.now += 10;
        f.node->updateTracking(0.1, double(f.now));
    }
    const double pulled = f.node->progress(double(f.now) + 100);
    EXPECT_GT(pulled, 1);
    EXPECT_LT(pulled, 1 + Node::kRubberBandEdge);
}

TEST(OverviewNode, AHeldReleaseGoesToTheNearerEndAndAFlickGoesOn)
{
    for (const bool flick : {false, true}) {
        SCOPED_TRACE(flick);
        Fixture f;
        f.node->startTracking();
        if (flick) {
            // A third of the way, fast.
            for (int step = 0; step < 4; ++step) {
                f.now += 8;
                f.node->updateTracking(0.3 / 4, double(f.now));
            }
        } else {
            // Past half way, then back to a third and held there.
            for (int step = 0; step < 6; ++step) {
                f.now += 8;
                f.node->updateTracking(0.1, double(f.now));
            }
            for (int step = 0; step < 6; ++step) {
                f.now += 8;
                f.node->updateTracking(-0.05, double(f.now));
            }
            f.now += 150;
        }
        EXPECT_EQ(flick ? 1 : 0, f.node->endTracking(double(f.now)));
        f.settle();
        EXPECT_EQ(QList<double>({flick ? 1.0 : 0.0}), f.finished);
    }
}

TEST(OverviewNode, ReleasedItCarriesOnAtTheFingersSpeed)
{
    Fixture f;
    f.node->startTracking();
    // Past half way slower than a flick: 0.4 a second towards the nearer end.
    for (int step = 0; step < 150; ++step) {
        f.now += 10;
        f.node->updateTracking(0.004, double(f.now));
    }
    EXPECT_EQ(1, f.node->endTracking(double(f.now)));
    const double now = double(f.now);
    const double speed = (f.node->progress(now + 0.1) - f.node->progress(now)) * 10000;
    EXPECT_NEAR(0.4, speed, 0.02);
    // On the quick curve: the rest of the way within the shorter time.
    f.now += qint64(Node::kShowDurationMs);
    f.paint();
    EXPECT_EQ(QList<double>({1}), f.finished);
    f.settle();
    EXPECT_EQ(QList<double>({1}), f.finished);
}

TEST(OverviewNode, TheFingersCatchTheTweenWhereItIsShown)
{
    Fixture f;
    f.node->animateTo(1);
    f.now += 120;
    const double before = f.node->progress(double(f.now));
    ASSERT_GT(before, 0.1);
    ASSERT_LT(before, 0.9);
    f.node->startTracking();
    EXPECT_NEAR(before, f.node->progress(double(f.now)), 1e-9);
    // And the shortcut catches the fingers the same way.
    f.node->updateTracking(0.1, double(f.now));
    f.now += 20;
    const double tracked = f.node->progress(double(f.now));
    f.node->animateTo(0);
    EXPECT_NEAR(tracked, f.node->progress(double(f.now)), 1e-9);
}

TEST(OverviewNode, ANewLayoutMovesWindowsThereOnATween)
{
    Fixture f;
    f.node->jumpTo(1);
    const QRectF moved(200, 150, 300, 225);
    f.node->setItems({f.item(moved)}, true);
    EXPECT_EQ(kPlace, f.rect(1));
    f.now += 100;
    const QRectF partway = f.rect(1);
    EXPECT_NE(kPlace, partway);
    EXPECT_NE(moved, partway);
    f.now += kSettleMs;
    EXPECT_EQ(moved, f.rect(1));

    // A dragged window heads home from where it was let go.
    const QRectF dragged(700, 50, 300, 225);
    f.node->setDragged(0, dragged);
    EXPECT_EQ(dragged, f.rect(1));
    f.node->setItems({f.item(moved)}, true);
    EXPECT_EQ(dragged, f.rect(1));
    f.now += kSettleMs;
    EXPECT_EQ(moved, f.rect(1));
}

TEST(OverviewNode, TheTitleShowsOnceItIsIn)
{
    Fixture f;
    int painted = 0;
    f.node->setTextPainter([&painted](const QString &, int, const render::Color &, double, int) {
        ++painted;
        QImage image(60, 16, QImage::Format_RGBA8888_Premultiplied);
        image.fill(Qt::white);
        return image;
    });
    f.node->setChrome({0, -1});
    f.node->animateTo(1);
    f.now += 16;
    f.paint();
    // Not while it moves.
    EXPECT_EQ(0, painted);
    f.settle();
    f.now += 1000;
    f.paint();
    EXPECT_EQ(1, painted);
    // In the middle of the window.
    const auto targets = [&] {
        QList<QRectF> result;
        for (const auto &call : f.renderer.calls) {
            if (call.kind == QStringLiteral("texture"))
                result.append(call.target);
        }
        return result;
    }();
    EXPECT_TRUE(std::any_of(targets.begin(), targets.end(), [](const QRectF &target) {
        return target.size() == QSizeF(60, 16) && kPlace.contains(target)
                && std::abs(target.center().x() - kPlace.center().x()) <= 1
                && std::abs(target.center().y() - kPlace.center().y()) <= 1;
    }));
}

TEST(OverviewNode, EveryWindowGetsTheOverviewsCornersShadowAndOutline)
{
    Fixture f;
    struct Drawn {
        QRectF body;
        qreal radius;
        double uniform;
        float opacity;
    };
    QList<Drawn> shadows;
    QList<Drawn> outlines;
    scene::OverviewWindowFrame frame;
    frame.radius = 14;
    frame.shadow = [&](render::Renderer &, const scene::WindowAnimation &,
                       const render::RoundedShape &body, double uniform, float opacity) {
        shadows.append({body.bounds, body.radius, uniform, opacity});
    };
    frame.outline = [&](render::Renderer &, const render::RoundedShape &body, float opacity) {
        outlines.append({body.bounds, body.radius, 1, opacity});
    };
    f.node->setWindowFrame(frame);

    f.node->jumpTo(1);
    f.paint();
    ASSERT_EQ(1, shadows.size());
    EXPECT_EQ(kPlace, shadows[0].body);
    // A normal window's corners, at the window's half size.
    EXPECT_EQ(7, shadows[0].radius);
    EXPECT_EQ(1, shadows[0].uniform);
    ASSERT_EQ(1, outlines.size());
    EXPECT_EQ(kPlace, outlines[0].body);
    EXPECT_FLOAT_EQ(1, outlines[0].opacity);

    // Halfway in, at three quarters of its size, the window's own square
    // corners are halfway to the overview's, and so is its outline.
    shadows.clear();
    outlines.clear();
    f.node->jumpTo(0.5);
    f.paint();
    ASSERT_EQ(1, shadows.size());
    EXPECT_DOUBLE_EQ(14 * 0.75 / 2, shadows[0].radius);
    EXPECT_DOUBLE_EQ(0.5, shadows[0].uniform);
    ASSERT_EQ(1, outlines.size());
    EXPECT_FLOAT_EQ(0.5, outlines[0].opacity);
}

TEST(OverviewNode, ADraggedWindowIsDrawnOverTheDeskBar)
{
    Fixture f;
    f.container->addChild(std::make_unique<scene::OverviewDeskBarNode>(f.node));
    f.node->jumpTo(1);
    const QRectF dragged(50, 20, 100, 75);
    f.node->setDragged(0, dragged);
    f.paint();
    QList<QRectF> targets;
    for (const auto &call : f.renderer.calls) {
        if (call.kind == QStringLiteral("texture"))
            targets.append(call.target);
    }
    // Once, by the bar, after everything the overview draws.
    ASSERT_EQ(1, targets.size());
    const double scale = dragged.width() / kWindow.width();
    const QRectF coverage = QRectF(kWindow).adjusted(-64, -64, 64, 64);
    EXPECT_EQ(QRectF(dragged.topLeft() + (coverage.topLeft() - kWindow.topLeft()) * scale,
                     coverage.size() * scale),
              targets[0]);
    EXPECT_EQ(QStringLiteral("texture"), f.renderer.calls.last().kind);
}
