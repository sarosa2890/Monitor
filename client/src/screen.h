#pragma once

#include <cstdint>
#include <vector>

struct ScreenCap {
    int width = 0, height = 0;
    int stride = 0;              // байт на строку (24bpp, выровнено на 4)
    std::vector<uint8_t> rgb;    // top-down RGB24
};

// Захват виртуального рабочего стола (все мониторы).
bool screenCapture(ScreenCap& out);
