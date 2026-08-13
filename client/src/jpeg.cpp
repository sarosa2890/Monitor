#include "jpeg.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")

using namespace Gdiplus;

static ULONG_PTR g_gdiToken = 0;
static bool g_gdiInit = false;

bool jpegInit() {
    if (g_gdiInit) return true;
    GdiplusStartupInput in;
    if (GdiplusStartup(&g_gdiToken, &in, nullptr) == Ok) {
        g_gdiInit = true;
        return true;
    }
    return false;
}

static CLSID encoderClsid(const WCHAR* mimeType) {
    CLSID clsid{};
    UINT num = 0, size = 0;
    if (GetImageEncodersSize(&num, &size) != Ok || size == 0) return clsid;
    std::vector<BYTE> buf(size);
    ImageCodecInfo* infos = (ImageCodecInfo*)buf.data();
    if (GetImageEncoders(num, size, infos) != Ok) return clsid;
    for (UINT i = 0; i < num; i++) {
        if (wcscmp(infos[i].MimeType, mimeType) == 0) {
            clsid = infos[i].Clsid;
            break;
        }
    }
    return clsid;
}

bool jpegEncodeRGB24(const uint8_t* rgb, int width, int height, int stride,
                     int quality, std::vector<uint8_t>& out) {
    if (!g_gdiInit || !rgb || width <= 0 || height <= 0) return false;
    if (quality < 5) quality = 5;
    if (quality > 100) quality = 100;

    Bitmap bmp(width, height, PixelFormat24bppRGB);
    Rect rc(0, 0, width, height);
    BitmapData bd;
    if (bmp.LockBits(&rc, ImageLockModeWrite, PixelFormat24bppRGB, &bd) != Ok)
        return false;

    // копируем по строкам с учётом возможного паддинга stride
    const uint8_t* src = rgb;
    uint8_t* dst = (uint8_t*)bd.Scan0;
    int dstStride = bd.Stride;
    if (dstStride < 0) dstStride = -dstStride;
    for (int y = 0; y < height; y++) {
        memcpy(dst + (size_t)y * dstStride, src + (size_t)y * stride, (size_t)width * 3);
    }
    bmp.UnlockBits(&bd);

    CLSID clsid = encoderClsid(L"image/jpeg");
    if (clsid == CLSID{}) return false;

    IStream* stm = nullptr;
    if (CreateStreamOnHGlobal(nullptr, TRUE, &stm) != S_OK) return false;

    EncoderParameters ep;
    ep.Count = 1;
    ep.Parameter[0].Guid = EncoderQuality;
    ep.Parameter[0].Type = EncoderParameterValueTypeLong;
    ep.Parameter[0].NumberOfValues = 1;
    ep.Parameter[0].Value = &quality;

    Status st = bmp.Save(stm, &clsid, &ep);
    if (st != Ok) {
        stm->Release();
        return false;
    }

    HGLOBAL hg = nullptr;
    if (GetHGlobalFromStream(stm, &hg) == S_OK && hg) {
        SIZE_T sz = GlobalSize(hg);
        const uint8_t* p = (const uint8_t*)GlobalLock(hg);
        if (p) {
            out.assign(p, p + sz);
            GlobalUnlock(hg);
        }
    }
    stm->Release();
    return !out.empty();
}
