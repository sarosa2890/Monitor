// Диагностика MF-захвата камеры: полный лог HRESULT по шагам.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdio.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wchar.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

static void hr(const char* step, HRESULT h) {
    printf("  %-42s -> 0x%08X%s\n", step, (unsigned)h, SUCCEEDED(h) ? "" : "  <<< FAIL");
}

static void printSubtypes(IMFSourceReader* r, UINT32 streamIndex) {
    IMFMediaType *t = nullptr, *cur = nullptr;
    printf("[supported subtypes]\n");
    r->GetCurrentMediaType(streamIndex, &cur);
    for (DWORD i = 0;; i++) {
        if (r->GetNativeMediaType(streamIndex, i, &t) != S_OK) break;
        GUID major = GUID_NULL, sub = GUID_NULL;
        t->GetGUID(MF_MT_MAJOR_TYPE, &major);
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        UINT32 w = 0, h = 0;
        t->GetUINT32(MF_MT_FRAME_SIZE, (UINT32*)&w); // не всегда размер
        printf("    native[%u] major=%s subtype=%d.%d.%d.%d\n", i,
               (major == MFMediaType_Video) ? "video" : "??",
               (sub.Data1 >> 24) & 0xFF, (sub.Data1 >> 16) & 0xFF,
               (sub.Data1 >> 8) & 0xFF, sub.Data1 & 0xFF);
        t->Release();
        t = nullptr;
        if (i > 40) break;
    }
    if (cur) { printf("    current subtype=...\n"); cur->Release(); }
}

int wmain() {
    HRESULT h = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    printf("CoInitializeEx: 0x%08X (%s)\n", (unsigned)h, SUCCEEDED(h) ? "ok" : "warn");
    h = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    printf("MFStartup:      0x%08X\n", (unsigned)h);
    if (FAILED(h)) return 1;

    IMFAttributes* attr = nullptr;
    h = MFCreateAttributes(&attr, 2);
    hr("MFCreateAttributes", h);
    if (SUCCEEDED(h)) h = attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    hr("SetGUID(VIDCAP)", h);

    IMFActivate** devs = nullptr;
    UINT32 count = 0;
    h = MFEnumDeviceSources(attr, &devs, &count);
    hr("MFEnumDeviceSources", h);
    printf("  devices found: %u\n", count);

    for (UINT32 d = 0; d < count; d++) {
        WCHAR* name = nullptr;
        UINT32 nlen = 0;
        if (SUCCEEDED(devs[d]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &nlen)))
            wprintf(L"  [%u] %s\n", d, name);
        else
            printf("  [%u] <no name>\n", d);
        if (name) CoTaskMemFree(name);
    }
    if (count == 0) { printf("NO CAMERA DEVICES! Check privacy permissions or driver.\n"); return 0; }

    int idx = 0;
    printf("---------- opening device %d ----------\n", idx);
    IMFMediaSource* src = nullptr;
    // проверяем lock на устройство (нужно для отладки если занято)
    UINT32 lock = 0;
    h = devs[idx]->GetUINT32(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_MAX_BUFFERS, &lock);
    hr("GetUINT32(MAX_BUFFERS)", h);
    h = devs[idx]->ActivateObject(IID_PPV_ARGS(&src));
    hr("ActivateObject", h);
    if (FAILED(h)) {
        printf("  cannot open device %d, trying all...\n", idx);
        for (UINT32 d = 0; d < count && !src; d++) {
            h = devs[d]->ActivateObject(IID_PPV_ARGS(&src));
            printf("  open[%u]: 0x%08X\n", d, (unsigned)h);
        }
    }
    if (!src) { printf("ALL DEVICES FAILED\n"); return 0; }

    IMFSourceReader* reader = nullptr;
    h = MFCreateSourceReaderFromMediaSource(src, nullptr, &reader);
    hr("MFCreateSourceReaderFromMediaSource", h);
    if (FAILED(h)) return 0;
    src->Release();

    printSubtypes(reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM);

    // попытка 1: RGB24
    IMFMediaType* mt = nullptr;
    MFCreateMediaType(&mt);
    mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB24);
    h = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mt);
    hr("SetCurrentMediaType(RGB24)", h);
    mt->Release();

    if (FAILED(h)) {
        // попытка 2: NV12
        MFCreateMediaType(&mt);
        mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        h = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mt);
        hr("SetCurrentMediaType(NV12)", h);
        mt->Release();
    }

    IMFMediaType* out = nullptr;
    if (SUCCEEDED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &out))) {
        UINT32 w = 0, hh = 0;
        MFGetAttributeSize(out, MF_MT_FRAME_SIZE, &w, &hh);
        GUID g = GUID_NULL;
        out->GetGUID(MF_MT_SUBTYPE, &g);
        printf("  current type: %ux%u subtype tag %c%c%c%c\n", w, hh,
               (char)(g.Data1 >> 24), (char)(g.Data1 >> 16), (char)(g.Data1 >> 8), (char)(g.Data1 & 0xFF));
        UINT32 fpsN = 0, fpsD = 0;
        MFGetAttributeRatio(out, MF_MT_FRAME_RATE, &fpsN, &fpsD);
        printf("  frame rate: %u/%u\n", fpsN, fpsD);
        out->Release();
    }

    DWORD flags = 0;
    LONGLONG ts = 0;
    IMFSample* sample = nullptr;
    h = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &sample);
    hr("ReadSample", h);
    printf("  flags=0x%X sample=%s\n", flags, sample ? "yes" : "no");
    if (sample) {
        IMFMediaBuffer* buf = nullptr;
        sample->ConvertToContiguousBuffer(&buf);
        BYTE* d = nullptr;
        DWORD clen = 0;
        if (SUCCEEDED(buf->Lock(&d, nullptr, &clen))) {
            printf("  frame bytes: %u, first 8: ", clen);
            for (int i = 0; i < 8; i++) printf("%02X ", d[i]);
            printf("\n");
            buf->Unlock();
        }
        buf->Release();
        sample->Release();
    }
    getchar();
    reader->Release();
    MFShutdown();
    CoUninitialize();
    return 0;
}