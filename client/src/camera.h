#pragma once

#include <cstdint>
#include <vector>

// Захват веб-камеры через MediaFoundation (SourceReader).
// Порядок согласования формата: NV12 (1280x720) -> NV12 (native size) ->
// RGB24 -> MJPG (декод через WIC). Покрывает практически любые вебки.
class Camera {
public:
    Camera() = default;
    ~Camera() { shutdown(); }

    // deviceIndex — индекс камеры из списка устройств (0 = первая).
    bool init(int deviceIndex);

    // Захват одного кадра. rgb — top-down RGB24, stride выровнен на 4.
    bool captureFrame(std::vector<uint8_t>& rgb, int& width, int& height, uint64_t& tsMs);

    void shutdown();

private:
    void* m_reader = nullptr; // IMFSourceReader
    int m_width = 0, m_height = 0;
    int m_stride = 0;         // stride источника
    int m_fmt = 0;            // 0=none, 1=NV12, 2=RGB24, 3=MJPG
    bool m_coinit = false;
};