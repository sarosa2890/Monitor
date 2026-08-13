#include "screen.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

bool screenCapture(ScreenCap& out) {
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (w <= 0 || h <= 0) return false;

    HDC hdcScreen = GetDC(nullptr);

    // Top-down DIB: отрицательный biHeight -> строки идут сверху вниз.
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP hbmp = CreateDIBSection(hdcScreen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbmp || !bits) {
        if (hbmp) DeleteObject(hbmp);
        ReleaseDC(nullptr, hdcScreen);
        return false;
    }

    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HGDIOBJ old = SelectObject(hdcMem, hbmp);

    // CAPTUREBLT — чтобы попадали также окна с layered-стилем
    BitBlt(hdcMem, 0, 0, w, h, hdcScreen, x, y, SRCCOPY | CAPTUREBLT);

    out.width = w;
    out.height = h;
    out.stride = ((w * 3 + 3) / 4) * 4;
    out.rgb.resize((size_t)out.stride * h);
    memcpy(out.rgb.data(), bits, out.rgb.size());

    SelectObject(hdcMem, old);
    DeleteObject(hbmp);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
    return true;
}
