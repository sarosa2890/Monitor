#include "camera.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objidl.h>
#include <wincodec.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

enum { CAMFMT_NONE = 0, CAMFMT_NV12 = 1, CAMFMT_RGB24 = 2, CAMFMT_MJPG = 3 };

// --------------------------------------------------------- NV12 -> RGB24 --
// BT.601 limited range (как у MF/YUY2-конвертеров). Быстрая скалярная версия.
static inline uint8_t clampByte(int v) {
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static void nv12ToRgb24(const uint8_t* nv, int w, int h, int stride,
                        uint8_t* rgb, int outStride) {
    const uint8_t* yPlane = nv;
    const uint8_t* uvPlane = nv + (size_t)stride * h;
    for (int y = 0; y < h; y++) {
        const uint8_t* yRow = yPlane + (size_t)y * stride;
        const uint8_t* uvRow = uvPlane + (size_t)(y >> 1) * stride;
        uint8_t* out = rgb + (size_t)y * outStride;
        for (int x = 0; x < w; x++) {
            int Y = yRow[x];
            int U = uvRow[(x >> 1) << 1];
            int V = uvRow[((x >> 1) << 1) + 1];
            int C = Y - 16, D = U - 128, E = V - 128;
            out[x * 3 + 0] = clampByte((298 * C + 409 * E + 128) >> 8);
            out[x * 3 + 1] = clampByte((298 * C - 100 * D - 208 * E + 128) >> 8);
            out[x * 3 + 2] = clampByte((298 * C + 516 * D + 128) >> 8);
        }
    }
}

// -------------------------------------------------- MJPG -> RGB24 (WIC) ---
static bool decodeJpegWic(const uint8_t* jpeg, size_t len, std::vector<uint8_t>& rgb,
                          int& w, int& h, int& stride) {
    IWICImagingFactory* fac = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&fac))))
        return false;

    IStream* stm = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stm))) { fac->Release(); return false; }
    ULONG written = 0;
    stm->Write(jpeg, (ULONG)len, &written);

    IWICBitmapDecoder* dec = nullptr;
    HRESULT hr = fac->CreateDecoderFromStream(stm, nullptr, WICDecodeMetadataCacheOnDemand, &dec);
    stm->Release();
    if (FAILED(hr)) { fac->Release(); return false; }

    IWICBitmapFrameDecode* fr = nullptr;
    hr = dec->GetFrame(0, &fr);
    if (FAILED(hr)) { dec->Release(); fac->Release(); return false; }

    hr = fr->GetSize((UINT*)&w, (UINT*)&h);
    stride = ((w * 3 + 3) / 4) * 4;
    if (SUCCEEDED(hr)) {
        rgb.assign((size_t)stride * h, 0);
        hr = fr->CopyPixels(nullptr, stride, (UINT)rgb.size(), rgb.data());
    }
    fr->Release();
    dec->Release();
    fac->Release();
    return SUCCEEDED(hr) && !rgb.empty();
}

// ------------------------------------------------------------------ init --
bool Camera::init(int deviceIndex) {
    shutdown();

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;
    m_coinit = true;

    hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(hr)) return false;

    IMFAttributes* pAttr = nullptr;
    hr = MFCreateAttributes(&pAttr, 2);
    if (FAILED(hr)) return false;
    pAttr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                   MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate** pDevs = nullptr;
    UINT32 count = 0;
    hr = MFEnumDeviceSources(pAttr, &pDevs, &count);
    pAttr->Release();
    if (FAILED(hr) || count == 0) return false;

    int idx = deviceIndex;
    if (idx < 0) idx = 0;
    if (idx >= (int)count) idx = (int)count - 1;

    IMFMediaSource* pSrc = nullptr;
    hr = pDevs[idx]->ActivateObject(IID_PPV_ARGS(&pSrc));
    for (UINT32 i = 0; i < count; i++) pDevs[i]->Release();
    CoTaskMemFree(pDevs);
    if (FAILED(hr)) return false;

    IMFSourceReader* pReader = nullptr;
    hr = MFCreateSourceReaderFromMediaSource(pSrc, nullptr, &pReader);
    if (FAILED(hr)) { pSrc->Release(); return false; }
    pSrc->Release(); // ридер держит собственную ссылку

    auto trySet = [&](const GUID& subtype, bool withSize) -> HRESULT {
        IMFMediaType* mt = nullptr;
        MFCreateMediaType(&mt);
        mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        mt->SetGUID(MF_MT_SUBTYPE, subtype);
        if (withSize) MFSetAttributeSize(mt, MF_MT_FRAME_SIZE, 1280, 720);
        HRESULT h = pReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mt);
        mt->Release();
        return h;
    };

    int fmt = CAMFMT_NONE;
    if (SUCCEEDED(trySet(MFVideoFormat_NV12, true)))        fmt = CAMFMT_NV12;
    else if (SUCCEEDED(trySet(MFVideoFormat_NV12, false)))  fmt = CAMFMT_NV12;
    else if (SUCCEEDED(trySet(MFVideoFormat_RGB24, false))) fmt = CAMFMT_RGB24;
    else if (SUCCEEDED(trySet(MFVideoFormat_MJPG, false)))  fmt = CAMFMT_MJPG;

    if (fmt == CAMFMT_NONE) { pReader->Release(); return false; }
    m_fmt = fmt;

    IMFMediaType* pOut = nullptr;
    if (SUCCEEDED(pReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pOut))) {
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(pOut, MF_MT_FRAME_SIZE, &w, &h);
        m_width = (int)w;
        m_height = (int)h;
        UINT32 stride = 0;
        if (FAILED(pOut->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) || stride == 0)
            stride = fmt == CAMFMT_RGB24 ? ((int)w * 3 + 3) / 4 * 4 : (int)w;
        m_stride = (int)stride;
        pOut->Release();
    }
    if (m_width <= 0 || m_height <= 0) { pReader->Release(); return false; }

    m_reader = pReader;
    return true;
}

// ------------------------------------------------------------- capture ----
bool Camera::captureFrame(std::vector<uint8_t>& rgb, int& width, int& height, uint64_t& tsMs) {
    IMFSourceReader* pReader = (IMFSourceReader*)m_reader;
    if (!pReader) return false;

    IMFSample* pSample = nullptr;
    DWORD flags = 0;
    HRESULT hr = pReader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                     nullptr, &flags, (LONGLONG*)&tsMs, &pSample);
    if (FAILED(hr)) return false;

    if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
        IMFMediaType* pMT = nullptr;
        if (SUCCEEDED(pReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pMT))) {
            UINT32 w = 0, h = 0;
            if (SUCCEEDED(MFGetAttributeSize(pMT, MF_MT_FRAME_SIZE, &w, &h))) {
                m_width = (int)w;
                m_height = (int)h;
            }
            UINT32 stride = 0;
            if (FAILED(pMT->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) || stride == 0)
                stride = m_fmt == CAMFMT_RGB24 ? ((int)w * 3 + 3) / 4 * 4 : (int)w;
            m_stride = (int)stride;
            pMT->Release();
        }
    }
    if (!pSample) return false;

    IMFMediaBuffer* pBuf = nullptr;
    hr = pSample->ConvertToContiguousBuffer(&pBuf);
    pSample->Release();
    if (FAILED(hr)) return false;

    BYTE* data = nullptr;
    DWORD curLen = 0;
    hr = pBuf->Lock(&data, nullptr, &curLen);
    if (FAILED(hr)) { pBuf->Release(); return false; }

    int w = m_width, h = m_height;
    int outStride = ((w * 3 + 3) / 4) * 4;
    rgb.assign((size_t)outStride * h, 0);

    if (m_fmt == CAMFMT_NV12) {
        // NV12 top-down: Y-плоскость, затем UV. Конвертируем сразу в RGB24.
        int srcStride = m_stride > 0 ? m_stride : w;
        size_t yPlane = (size_t)srcStride * h;           // смещение UV от начала
        if (yPlane + (size_t)srcStride * (h >> 1) <= curLen)
            nv12ToRgb24(data, w, h, srcStride, rgb.data(), outStride);
    } else if (m_fmt == CAMFMT_RGB24) {
        // RGB24 bottom-up: переворачиваем строки.
        int srcStride = m_stride > 0 ? m_stride : outStride;
        int rows = (int)curLen / srcStride;
        if (rows > h) rows = h;
        for (int y = 0; y < rows; y++) {
            memcpy(rgb.data() + (size_t)(h - 1 - y) * outStride,
                   data + (size_t)y * srcStride, (size_t)outStride);
        }
    } else if (m_fmt == CAMFMT_MJPG) {
        int ww = 0, hh = 0, s = 0;
        std::vector<uint8_t> dec;
        if (decodeJpegWic(data, curLen, dec, ww, hh, s) && ww == w && hh == h)
            rgb.swap(dec);
    }

    pBuf->Unlock();
    pBuf->Release();

    width = w;
    height = h;
    return true;
}

void Camera::shutdown() {
    if (m_reader) {
        ((IMFSourceReader*)m_reader)->Release();
        m_reader = nullptr;
    }
    MFShutdown();
    if (m_coinit) {
        CoUninitialize();
        m_coinit = false;
    }
}
