// psdthumb.exe - command-line companion for testing the decoder and the registered shell handler.
#include "../src/psdthumb.h"
#include "../src/PsdDecoder.h"
#include "../src/WicHelpers.h"
#include <shlwapi.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

int Usage() {
    fputs("psdthumb - PSD/PSB thumbnail tool\n"
          "  psdthumb info   <file.psd>\n"
          "  psdthumb decode <file.psd> <out.png> [--size N] [--probe] [--no-fallback]\n"
          "  psdthumb shell  <file.psd> <out.png> [--size N] [--probe]\n"
          "      (asks the shell for the thumbnail, i.e. goes through the registered handler)\n",
          stderr);
    return 1;
}

struct Options {
    uint32_t size = 256;
    bool probe = false;
    bool fallback = true;
};

bool ParseOptions(int argc, wchar_t** argv, int first, Options& o) {
    for (int i = first; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--size" && i + 1 < argc) {
            o.size = (uint32_t)_wtoi(argv[++i]);
            if (o.size == 0) return false;
        } else if (a == L"--probe") {
            o.probe = true;
        } else if (a == L"--no-fallback") {
            o.fallback = false;
        } else {
            return false;
        }
    }
    return true;
}

// Prints the colour at 1/4 and 3/4 of the width on the middle row; the test-suite parses this.
void PrintProbe(const DecodedImage& img) {
    auto px = [&](uint32_t x, uint32_t y) { return img.bgra.data() + ((size_t)y * img.width + x) * 4; };
    const uint8_t* l = px(img.width / 4, img.height / 2);
    const uint8_t* r = px(img.width * 3 / 4, img.height / 2);
    printf("PROBE %u %u L %u %u %u %u R %u %u %u %u\n", img.width, img.height,
           l[2], l[1], l[0], l[3], r[2], r[1], r[0], r[3]);
}

bool OpenStream(const wchar_t* path, ComPtr<IStream>& s) {
    const HRESULT hr = SHCreateStreamOnFileEx(path, STGM_READ | STGM_SHARE_DENY_NONE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &s);
    if (FAILED(hr)) {
        fprintf(stderr, "cannot open file (hr=0x%08lx)\n", (unsigned long)hr);
        return false;
    }
    return true;
}

double MsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

int CmdInfo(const wchar_t* path) {
    ComPtr<IStream> s;
    if (!OpenStream(path, s)) return 2;
    Reader r(s.Get());
    PsdInfo info;
    const PsdResult res = PsdDecoder::ReadInfo(r, info);
    if (res != PsdResult::Ok) {
        printf("header: %s\n", PsdResultName(res));
        return 2;
    }
    printf("format:            %s (version %u)\n", info.IsPsb() ? "PSB" : "PSD", info.version);
    printf("size:              %u x %u\n", info.width, info.height);
    printf("channels:          %u\n", info.channels);
    printf("depth:             %u bit\n", info.depth);
    printf("color mode:        %s (%u)\n", PsdColorModeName(info.colorMode), info.colorMode);
    printf("compression:       %u (%s)\n", info.compression,
           info.compression == 0 ? "raw" : info.compression == 1 ? "RLE" : "zip");
    printf("merged alpha:      %s\n", info.hasMergedTransparency ? "yes" : "no");
    printf("real merged data:  %s%s\n", info.hasRealMergedData ? "yes" : "no",
           info.hasVersionInfo ? "" : " (no version info resource, assumed)");
    if (info.jpegSize)
        printf("jpeg thumbnail:    %u x %u, %u bytes%s\n", info.jpegWidth, info.jpegHeight, info.jpegSize,
               info.jpegIsBgr ? " (legacy BGR)" : "");
    else
        printf("jpeg thumbnail:    none\n");
    printf("image data offset: %llu\n", (unsigned long long)info.imageDataOffset);
    return 0;
}

int CmdDecode(const wchar_t* path, const wchar_t* outPng, const Options& o) {
    ComPtr<IStream> s;
    if (!OpenStream(path, s)) return 2;
    const auto t0 = std::chrono::steady_clock::now();
    Reader r(s.Get());
    PsdInfo info;
    const PsdResult ir = PsdDecoder::ReadInfo(r, info);
    if (ir != PsdResult::Ok) {
        fprintf(stderr, "header: %s\n", PsdResultName(ir));
        return 2;
    }
    DecodedImage img;
    const PsdResult dr = PsdDecoder::DecodeComposite(r, info, o.size, img);
    bool ok = dr == PsdResult::Ok;
    const char* source = "composite";
    if (!ok) {
        fprintf(stderr, "composite: %s\n", PsdResultName(dr));
        if (o.fallback) {
            std::vector<uint8_t> jpeg;
            if (PsdDecoder::ReadJpegThumbnail(r, info, jpeg)) {
                ok = WicDecodeImage(jpeg.data(), jpeg.size(), o.size, info.jpegIsBgr, img);
                source = "embedded jpeg";
            }
        }
    }
    if (!ok) {
        fprintf(stderr, "no thumbnail available\n");
        return 2;
    }
    const double ms = MsSince(t0);
    if (!WicSavePng(img, outPng)) {
        fprintf(stderr, "failed to write %ls\n", outPng);
        return 2;
    }
    printf("%ux%u from %s, alpha=%s, %.1f ms\n", img.width, img.height, source, img.hasAlpha ? "yes" : "no", ms);
    if (o.probe) PrintProbe(img);
    return 0;
}

bool ImageFromHBitmap(HBITMAP hbm, DecodedImage& img) {
    BITMAP bm = {};
    if (!GetObjectW(hbm, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight == 0) return false;
    const uint32_t w = (uint32_t)bm.bmWidth, h = (uint32_t)(bm.bmHeight < 0 ? -bm.bmHeight : bm.bmHeight);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = (LONG)w;
    bmi.bmiHeader.biHeight = -(LONG)h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    img.width = w;
    img.height = h;
    img.bgra.resize((size_t)w * h * 4);
    HDC dc = GetDC(nullptr);
    const int lines = GetDIBits(dc, hbm, 0, h, img.bgra.data(), &bmi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    if (lines != (int)h) return false;
    bool anyAlpha = false;
    for (size_t i = 3; i < img.bgra.size(); i += 4)
        if (img.bgra[i] != 0) { anyAlpha = true; break; }
    if (!anyAlpha)  // WTSAT_RGB bitmaps often come back with a zero alpha channel
        for (size_t i = 3; i < img.bgra.size(); i += 4) img.bgra[i] = 255;
    img.hasAlpha = anyAlpha;
    return true;
}

int CmdShell(const wchar_t* path, const wchar_t* outPng, const Options& o) {
    wchar_t full[MAX_PATH];
    if (!GetFullPathNameW(path, MAX_PATH, full, nullptr)) return 2;
    ComPtr<IShellItemImageFactory> factory;
    HRESULT hr = SHCreateItemFromParsingName(full, nullptr, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        fprintf(stderr, "SHCreateItemFromParsingName failed (hr=0x%08lx)\n", (unsigned long)hr);
        return 2;
    }
    const SIZE sz = { (LONG)o.size, (LONG)o.size };
    HBITMAP hbm = nullptr;
    const auto t0 = std::chrono::steady_clock::now();
    hr = factory->GetImage(sz, SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK, &hbm);
    const double ms = MsSince(t0);
    if (FAILED(hr) || !hbm) {
        fprintf(stderr, "shell GetImage failed (hr=0x%08lx) - is the handler registered?\n", (unsigned long)hr);
        return 2;
    }
    DecodedImage img;
    const bool ok = ImageFromHBitmap(hbm, img);
    DeleteObject(hbm);
    if (!ok) {
        fprintf(stderr, "could not read the shell bitmap\n");
        return 2;
    }
    if (!WicSavePng(img, outPng)) {
        fprintf(stderr, "failed to write %ls\n", outPng);
        return 2;
    }
    // If the handler DLL never got mapped into this process, the shell ran it in its COM surrogate.
    const bool inProc = GetModuleHandleW(L"psdthumb.dll") != nullptr;
    printf("%ux%u from shell, %.1f ms, handler ran %s\n", img.width, img.height, ms,
           inProc ? "in-process" : "out-of-process (COM surrogate)");
    if (o.probe) PrintProbe(img);
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) return Usage();
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        fputs("CoInitializeEx failed\n", stderr);
        return 2;
    }
    const std::wstring cmd = argv[1];
    Options o;
    int rc;
    if (cmd == L"info") rc = argc == 3 ? CmdInfo(argv[2]) : Usage();
    else if (cmd == L"decode") rc = (argc >= 4 && ParseOptions(argc, argv, 4, o)) ? CmdDecode(argv[2], argv[3], o) : Usage();
    else if (cmd == L"shell") rc = (argc >= 4 && ParseOptions(argc, argv, 4, o)) ? CmdShell(argv[2], argv[3], o) : Usage();
    else rc = Usage();
    CoUninitialize();
    return rc;
}
