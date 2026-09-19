#include "WicHelpers.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace {
HRESULT CreateFactory(ComPtr<IWICImagingFactory>& f) {
    return CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
}
}  // namespace

bool WicDecodeImage(const uint8_t* data, size_t len, uint32_t maxEdge, bool swapRB, DecodedImage& out) {
    if (!data || len == 0 || len > 0xFFFFFFFFull) return false;
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CreateFactory(f))) return false;
    ComPtr<IWICStream> stream;
    if (FAILED(f->CreateStream(&stream))) return false;
    if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(data), (DWORD)len))) return false;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec))) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return false;
    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0) return false;

    ComPtr<IWICBitmapSource> src;
    if (FAILED(frame.As(&src))) return false;
    if (maxEdge > 0 && std::max(w, h) > maxEdge) {
        const double s = (double)maxEdge / std::max(w, h);
        const UINT tw = std::max(1u, (UINT)(w * s + 0.5)), th = std::max(1u, (UINT)(h * s + 0.5));
        ComPtr<IWICBitmapScaler> sc;
        if (FAILED(f->CreateBitmapScaler(&sc))) return false;
        if (FAILED(sc->Initialize(src.Get(), tw, th, WICBitmapInterpolationModeFant))) return false;
        if (FAILED(sc.As(&src))) return false;
        w = tw;
        h = th;
    }
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(f->CreateFormatConverter(&conv))) return false;
    if (FAILED(conv->Initialize(src.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom)))
        return false;

    out.width = w;
    out.height = h;
    out.hasAlpha = false;
    out.bgra.resize((size_t)w * h * 4);
    if (FAILED(conv->CopyPixels(nullptr, w * 4, (UINT)out.bgra.size(), out.bgra.data()))) return false;
    if (swapRB)
        for (size_t i = 0; i < out.bgra.size(); i += 4) std::swap(out.bgra[i], out.bgra[i + 2]);
    return true;
}

bool WicSavePng(const DecodedImage& img, const wchar_t* path) {
    if (img.width == 0 || img.height == 0 || img.bgra.size() < (size_t)img.width * img.height * 4) return false;
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CreateFactory(f))) return false;
    ComPtr<IWICStream> stream;
    if (FAILED(f->CreateStream(&stream))) return false;
    if (FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE))) return false;
    ComPtr<IWICBitmapEncoder> enc;
    if (FAILED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc))) return false;
    if (FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
    ComPtr<IWICBitmapFrameEncode> fr;
    ComPtr<IPropertyBag2> props;
    if (FAILED(enc->CreateNewFrame(&fr, &props))) return false;
    if (FAILED(fr->Initialize(props.Get()))) return false;
    if (FAILED(fr->SetSize(img.width, img.height))) return false;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(fr->SetPixelFormat(&fmt))) return false;
    if (FAILED(fr->WritePixels(img.height, img.width * 4, (UINT)img.bgra.size(), const_cast<BYTE*>(img.bgra.data()))))
        return false;
    return SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
}

HBITMAP CreateDibFromImage(const DecodedImage& img) {
    if (img.width == 0 || img.height == 0 || img.bgra.size() < (size_t)img.width * img.height * 4) return nullptr;
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = (LONG)img.width;
    bmi.bmiHeader.biHeight = -(LONG)img.height;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP hbm = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbm || !bits) {
        if (hbm) DeleteObject(hbm);
        return nullptr;
    }
    uint8_t* dst = static_cast<uint8_t*>(bits);
    const uint8_t* src = img.bgra.data();
    const size_t n = (size_t)img.width * img.height;
    if (img.hasAlpha) {
        for (size_t i = 0; i < n; ++i, src += 4, dst += 4) {
            const uint32_t a = src[3];
            dst[0] = (uint8_t)((src[0] * a + 127) / 255);
            dst[1] = (uint8_t)((src[1] * a + 127) / 255);
            dst[2] = (uint8_t)((src[2] * a + 127) / 255);
            dst[3] = (uint8_t)a;
        }
    } else {
        memcpy(dst, src, n * 4);
        for (size_t i = 3; i < n * 4; i += 4) static_cast<uint8_t*>(bits)[i] = 255;
    }
    return hbm;
}
