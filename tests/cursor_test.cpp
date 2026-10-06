// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gtest/gtest.h>

#include <QCoreApplication>

#include "compositor/compositor.h"
#include "cursor/cursor.h"
#include "input/input_manager.h"
#include "input/seat.h"
#include "scene/scene.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

using namespace cutefish;

TEST(CursorTheme, EveryStandardShapeLoadsWithPremultipliedPixels)
{
    for (int value = 1; value <= 34; ++value) {
        for (int size : {24, 32, 48, 64}) {
            const auto frames = cursor::load(static_cast<input::CursorShape>(value), size);
            ASSERT_FALSE(frames.empty()) << value << " size " << size;
            for (const auto &frame : frames) {
                EXPECT_FALSE(frame.image.isNull());
                EXPECT_TRUE(frame.image.rect().contains(frame.hotspot));
                bool visible = false;
                for (int y = 0; y < frame.image.height(); ++y) {
                    for (int x = 0; x < frame.image.width(); ++x) {
                        const auto pixel = frame.image.pixel(x, y);
                        EXPECT_LE(qRed(pixel), qAlpha(pixel));
                        EXPECT_LE(qGreen(pixel), qAlpha(pixel));
                        EXPECT_LE(qBlue(pixel), qAlpha(pixel));
                        visible |= qAlpha(pixel) != 0;
                    }
                }
                EXPECT_TRUE(visible);
            }
        }
    }
    EXPECT_TRUE(cursor::load(input::CursorShape::Hidden, 24).empty());
}

namespace {
QByteArray cursorFile()
{
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::LittleEndian);
    out << quint32(0x72756358) << quint32(16) << quint32(1) << quint32(1);
    out << quint32(0xfffd0002) << quint32(24) << quint32(28);
    out << quint32(36) << quint32(0xfffd0002) << quint32(24) << quint32(1);
    out << quint32(2) << quint32(1) << quint32(1) << quint32(0) << quint32(37);
    out << quint32(0xff123456) << quint32(0xffabcdef);
    return bytes;
}

class CursorSearchTest : public testing::Test
{
protected:
    void SetUp() override { qputenv("XCURSOR_PATH", directory.path().toUtf8()); }

    void TearDown() override
    {
        if (previous.isNull())
            qunsetenv("XCURSOR_PATH");
        else
            qputenv("XCURSOR_PATH", previous);
    }

    QByteArray previous = qgetenv("XCURSOR_PATH");
    QTemporaryDir directory;
};
} // namespace

TEST(CursorTheme, ReadsPixelsHotspotAndDelay)
{
    const auto frames = cursor::readXCursor(cursorFile(), 24);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0].image.size(), QSize(2, 1));
    EXPECT_EQ(frames[0].image.pixel(0, 0), 0xff123456u);
    EXPECT_EQ(frames[0].image.pixel(1, 0), 0xffabcdefu);
    EXPECT_EQ(frames[0].hotspot, QPoint(1, 0));
    EXPECT_EQ(frames[0].delayMs, 37);
}

TEST(CursorTheme, RejectsTruncatedAndOversizedFiles)
{
    const auto bytes = cursorFile();
    for (int size = 0; size < bytes.size(); ++size)
        EXPECT_TRUE(cursor::readXCursor(bytes.left(size), 24).empty());
    auto corrupt = bytes;
    corrupt[12] = char(0xff);
    corrupt[13] = char(0xff);
    EXPECT_TRUE(cursor::readXCursor(corrupt, 24).empty());
    corrupt = bytes;
    corrupt[45] = char(0xff);
    EXPECT_TRUE(cursor::readXCursor(corrupt, 24).empty());
}

TEST_F(CursorSearchTest, LoadsAdwaitaFromSearchPathAndFallsBackToPointer)
{
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(QDir(directory.path()).mkpath("Adwaita/cursors"));
    QFile file(directory.filePath("Adwaita/cursors/left_ptr"));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write(cursorFile()), cursorFile().size());
    file.close();
    const auto frames = cursor::load(input::CursorShape::Wait, 24);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0].image.pixel(0, 0), 0xff123456u);
    EXPECT_TRUE(cursor::load(input::CursorShape::Hidden, 24).empty());
}

TEST_F(CursorSearchTest, MissingThemeKeepsPointerVisible)
{
    const auto frames = cursor::load(input::CursorShape::Default, 24);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames.front().image.size(), QSize(24, 24));
    EXPECT_FALSE(frames.front().image.isNull());
}

TEST_F(CursorSearchTest, ResolvesInheritanceAndLegacyShapeAliases)
{
    ASSERT_TRUE(QDir(directory.path()).mkpath("Parent/cursors"));
    ASSERT_TRUE(QDir(directory.path()).mkpath("Child"));
    QFile index(directory.filePath("Child/index.theme"));
    ASSERT_TRUE(index.open(QIODevice::WriteOnly));
    index.write("[Icon Theme]\nInherits=Parent\n");
    index.close();
    QFile file(directory.filePath("Parent/cursors/hand2"));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(cursorFile());
    file.close();
    const auto frames = cursor::load(input::CursorShape::Pointer, 24, QStringLiteral("Child"));
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0].image.pixel(0, 0), 0xff123456u);
}

TEST_F(CursorSearchTest, CyclicInheritanceAndInvalidNamesFallBack)
{
    ASSERT_TRUE(QDir(directory.path()).mkpath("Loop"));
    QFile index(directory.filePath("Loop/index.theme"));
    ASSERT_TRUE(index.open(QIODevice::WriteOnly));
    index.write("[Icon Theme]\nInherits=Loop\n");
    index.close();
    EXPECT_FALSE(cursor::load(input::CursorShape::Default, 24, QStringLiteral("Loop")).empty());
    for (const auto &name :
         {QStringLiteral("../escape"), QStringLiteral("/absolute"), QStringLiteral("..")}) {
        EXPECT_FALSE(cursor::validThemeName(name));
        EXPECT_FALSE(cursor::load(input::CursorShape::Default, 24, name).empty());
    }
}

TEST(CursorScene, PhysicalPositionAndOverrideUseIndependentClientLayer)
{
    int argc = 1;
    char name[] = "cursor-test";
    char *argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    scene::Scene scene;
    auto *clients = scene.root()->addChild(std::make_unique<scene::Node>());
    compositor::Compositor compositor(nullptr);
    input::Seat seat(&compositor);
    seat.setCursorContainer(clients);
    input::InputManager input(&seat);
    cursor::Cursor cursor(seat, *scene.root(), 24);
    auto *node = scene.root()->children().back();
    input.warpPointer({100, 90});
    EXPECT_EQ(node->position(),
              QPoint(100, 90) - cursor::load(input::CursorShape::Default, 24).front().hotspot);
    EXPECT_TRUE(node->isVisible());
    seat.setCompositorCursorShape(input::CursorShape::Hidden);
    EXPECT_FALSE(node->isVisible());
    seat.setCursorOverride(input::CursorShape::EwResize);
    EXPECT_TRUE(node->isVisible());
    EXPECT_FALSE(clients->isVisible());
    seat.setCursorOverride(std::nullopt);
    EXPECT_TRUE(clients->isVisible());
    EXPECT_FALSE(node->isVisible());
}

TEST(CursorScene, AHeldCursorLeavesThePlaneAloneUntilReleased)
{
    // A compositor starting up keeps off the plane, where the previous one's
    // cursor still is, until its displays settle; then it takes it over once.
    int argc = 1;
    char name[] = "cursor-held-test";
    char *argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    class Sink : public render::CursorSink
    {
    public:
        bool setCursorImage(const render::CursorImage &image) override
        {
            ++images;
            hotspot = image.hotspot;
            return true;
        }

        void hideCursor() override { ++hides; }

        void moveCursor(const QPointF &position) override { moves.push_back(position); }

        int images = 0;
        int hides = 0;
        QPoint hotspot;
        std::vector<QPointF> moves;
    } sink;
    scene::Scene scene;
    compositor::Compositor compositor(nullptr);
    input::Seat seat(&compositor);
    input::InputManager input(&seat);
    cursor::Cursor cursor(seat, *scene.root(), 24);
    auto *node = scene.root()->children().back();
    cursor.setHeld(true);
    EXPECT_FALSE(node->isVisible());
    cursor.setSink(&sink);
    cursor.setScale(1.5);
    input.warpPointer({100, 90});
    cursor.refresh();
    EXPECT_EQ(sink.images, 0);
    EXPECT_EQ(sink.hides, 0);
    EXPECT_TRUE(sink.moves.empty());
    EXPECT_FALSE(node->isVisible());
    cursor.setHeld(false);
    EXPECT_EQ(sink.images, 1);
    EXPECT_EQ(sink.hides, 0);
    // What the takeover adds to the old image's corner is what the plane takes off.
    EXPECT_EQ(cursor.deviceHotspot(), sink.hotspot);
    ASSERT_FALSE(sink.moves.empty());
    EXPECT_EQ(sink.moves.back(), QPointF(100, 90));
    EXPECT_TRUE(cursor.isHardware());
    EXPECT_FALSE(node->isVisible());
}

TEST_F(CursorSearchTest, SwitchingThemeRefreshesHardwareImageWithoutPointerMotion)
{
    int argc = 1;
    char name[] = "cursor-theme-test";
    char *argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    for (const auto &theme : {QStringLiteral("First"), QStringLiteral("Second")}) {
        ASSERT_TRUE(QDir(directory.path()).mkpath(theme + "/cursors"));
        QFile file(directory.filePath(theme + "/cursors/left_ptr"));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        auto bytes = cursorFile();
        if (theme == "Second")
            bytes[bytes.size() - 8] = char(0x78);
        file.write(bytes);
    }

    class Sink : public render::CursorSink
    {
    public:
        bool setCursorImage(const render::CursorImage &frame) override
        {
            image = QImage(frame.pixels, frame.size.width(), frame.size.height(), frame.stride,
                           QImage::Format_ARGB32_Premultiplied)
                            .copy();
            return true;
        }

        void hideCursor() override { }

        void moveCursor(const QPointF &) override { }

        QImage image;
    } sink;

    scene::Scene scene;
    compositor::Compositor compositor(nullptr);
    input::Seat seat(&compositor);
    cursor::Cursor cursor(seat, *scene.root(), 24);
    cursor.setSink(&sink);
    cursor.setTheme("First", 24);
    ASSERT_TRUE(cursor.isHardware());
    EXPECT_EQ(sink.image.pixel(0, 0), 0xff123456u);
    cursor.setTheme("Second", 24);
    EXPECT_EQ(sink.image.pixel(0, 0), 0xff123478u);
    cursor.setTheme("First", 24);
    EXPECT_EQ(sink.image.pixel(0, 0), 0xff123456u);
    cursor.setTheme("Missing", 48);
    EXPECT_EQ(sink.image.size(), QSize(48, 48));
}

TEST_F(CursorSearchTest, AShapeIsReadOncePerThemeAndSize)
{
    int argc = 1;
    char name[] = "cursor-cache-test";
    char *argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    const auto write = [&](const QString &theme, char blue) {
        ASSERT_TRUE(QDir(directory.path()).mkpath(theme + "/cursors"));
        QFile file(directory.filePath(theme + "/cursors/left_ptr"));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        auto bytes = cursorFile();
        bytes[bytes.size() - 8] = blue;
        file.write(bytes);
    };
    write("First", char(0x56));
    write("Second", char(0x78));

    class Sink : public render::CursorSink
    {
    public:
        bool setCursorImage(const render::CursorImage &frame) override
        {
            image = QImage(frame.pixels, frame.size.width(), frame.size.height(), frame.stride,
                           QImage::Format_ARGB32_Premultiplied)
                            .copy();
            return true;
        }

        void hideCursor() override { }

        void moveCursor(const QPointF &) override { }

        QImage image;
    } sink;

    scene::Scene scene;
    compositor::Compositor compositor(nullptr);
    input::Seat seat(&compositor);
    cursor::Cursor cursor(seat, *scene.root(), 24);
    cursor.setSink(&sink);
    cursor.setTheme("First", 24);
    ASSERT_TRUE(cursor.isHardware());
    EXPECT_EQ(sink.image.pixel(0, 0), 0xff123456u);
    write("First", char(0x9a));
    // A shape not shown yet is read now; one shown before is not read again.
    seat.setCompositorCursorShape(input::CursorShape::Pointer);
    EXPECT_EQ(sink.image.pixel(0, 0), 0xff12349au);
    seat.setCompositorCursorShape(input::CursorShape::Default);
    EXPECT_EQ(sink.image.pixel(0, 0), 0xff123456u);
    cursor.setTheme("Second", 24);
    EXPECT_EQ(sink.image.pixel(0, 0), 0xff123478u);
    cursor.setTheme("First", 24);
    EXPECT_EQ(sink.image.pixel(0, 0), 0xff12349au);
}
