#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// The retail HUD is authored in 640x480 pixels. Keep its height proportional
// to the window, expanding the logical width for widescreen displays. Input
// and drawing use this same canvas; the swapchain presents it at window size.
struct SGameCanvas
{
    int32_t width = 640, height = 480;

    static SGameCanvas ForWindow(int32_t w, int32_t h, bool scaleHud = true)
    {
        w = (std::max)(w, 1);
        h = (std::max)(h, 1);
        if (!scaleHud) return {w, h};
        const double scale = (std::min)(double(w) / 640.0, double(h) / 480.0);
        return {int32_t(std::lround(w / scale)), int32_t(std::lround(h / scale))};
    }

    int32_t MouseX(float x, int32_t windowWidth) const
    { return int32_t(std::floor(double(x) * width / (std::max)(windowWidth, 1))); }
    int32_t MouseY(float y, int32_t windowHeight) const
    { return int32_t(std::floor(double(y) * height / (std::max)(windowHeight, 1))); }
};
