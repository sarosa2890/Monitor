#pragma once

#include <cstdint>
#include <vector>

// Инициализация GDI+ (вызвать один раз).
bool jpegInit();

// Кодирование RGB24 (top-down, stride байт на строку) в JPEG.
// quality: 5..100. Результат кладётся в out.
bool jpegEncodeRGB24(const uint8_t* rgb, int width, int height, int stride,
                     int quality, std::vector<uint8_t>& out);
