#include "../src/gamecanvas.h"
#include <gtest/gtest.h>

TEST(GameCanvas, KeepsRetailCoordinatesAtNativeSize)
{
    const auto canvas = SGameCanvas::ForWindow(640, 480);
    EXPECT_EQ(canvas.width, 640);
    EXPECT_EQ(canvas.height, 480);
    EXPECT_EQ(canvas.MouseX(320, 640), 320);
    EXPECT_EQ(canvas.MouseY(240, 480), 240);
}

TEST(GameCanvas, ScalesHudWithHeightAndAddsWidescreenCoverage)
{
    const auto canvas = SGameCanvas::ForWindow(1920, 1080);
    EXPECT_EQ(canvas.width, 853);
    EXPECT_EQ(canvas.height, 480);
    EXPECT_EQ(canvas.MouseX(960, 1920), 426);
    EXPECT_EQ(canvas.MouseY(540, 1080), 240);
    EXPECT_EQ(canvas.MouseX(1919, 1920), 852);
    const auto ultrawide = SGameCanvas::ForWindow(5120, 1440);
    EXPECT_EQ(ultrawide.width, 1707);
    EXPECT_EQ(ultrawide.height, 480);
}

TEST(GameCanvas, FitsSmallAndPortraitWindows)
{
    const auto small = SGameCanvas::ForWindow(320, 240);
    EXPECT_EQ(small.width, 640);
    EXPECT_EQ(small.height, 480);
    const auto portrait = SGameCanvas::ForWindow(480, 800);
    EXPECT_EQ(portrait.width, 640);
    EXPECT_EQ(portrait.height, 1067);
    EXPECT_EQ(portrait.MouseY(799, 800), 1065);
}

TEST(GameCanvas, EditorUsesWindowPixels)
{
    const auto canvas = SGameCanvas::ForWindow(1920, 1080, false);
    EXPECT_EQ(canvas.width, 1920);
    EXPECT_EQ(canvas.height, 1080);
    EXPECT_EQ(canvas.MouseX(1234, 1920), 1234);
    EXPECT_EQ(canvas.MouseY(987, 1080), 987);
}
