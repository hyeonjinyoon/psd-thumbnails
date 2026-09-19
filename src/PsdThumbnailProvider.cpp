#include "PsdThumbnailProvider.h"
#include "PsdDecoder.h"
#include "WicHelpers.h"
#include <shlwapi.h>
#include <algorithm>
#include <new>
#include <vector>

extern volatile LONG g_cDllRef;

PsdThumbnailProvider::PsdThumbnailProvider() : m_ref(1), m_stream(nullptr) {
    InterlockedIncrement(&g_cDllRef);
}

PsdThumbnailProvider::~PsdThumbnailProvider() {
    if (m_stream) m_stream->Release();
    InterlockedDecrement(&g_cDllRef);
}

IFACEMETHODIMP PsdThumbnailProvider::QueryInterface(REFIID riid, void** ppv) {
    static const QITAB qit[] = {
        QITABENT(PsdThumbnailProvider, IInitializeWithStream),
        QITABENT(PsdThumbnailProvider, IThumbnailProvider),
        { nullptr, 0 },
    };
    return QISearch(this, qit, riid, ppv);
}

IFACEMETHODIMP_(ULONG) PsdThumbnailProvider::AddRef() {
    return InterlockedIncrement(&m_ref);
}

IFACEMETHODIMP_(ULONG) PsdThumbnailProvider::Release() {
    const ULONG ref = InterlockedDecrement(&m_ref);
    if (ref == 0) delete this;
    return ref;
}

IFACEMETHODIMP PsdThumbnailProvider::Initialize(IStream* pStream, DWORD /*grfMode*/) {
    if (!pStream) return E_POINTER;
    if (m_stream) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
    m_stream = pStream;
    m_stream->AddRef();
    return S_OK;
}

IFACEMETHODIMP PsdThumbnailProvider::GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* pdwAlpha) {
    if (!phbmp || !pdwAlpha) return E_POINTER;
    *phbmp = nullptr;
    *pdwAlpha = WTSAT_UNKNOWN;
    if (!m_stream) return E_UNEXPECTED;

    try {
        const uint32_t maxEdge = std::min<uint32_t>(cx ? cx : 256, PSDTHUMB_MAX_EDGE);
        Reader r(m_stream);
        PsdInfo info;
        const PsdResult ir = PsdDecoder::ReadInfo(r, info);
        PsdLog("GetThumbnail cx=%u: header=%s %ux%u ch=%u depth=%u mode=%s comp=%u alpha=%d real=%d jpeg=%u",
               cx, PsdResultName(ir), info.width, info.height, info.channels, info.depth,
               PsdColorModeName(info.colorMode), info.compression, info.hasMergedTransparency ? 1 : 0,
               info.hasRealMergedData ? 1 : 0, info.jpegSize);
        if (ir != PsdResult::Ok) return E_FAIL;

        DecodedImage img;
        const PsdResult dr = PsdDecoder::DecodeComposite(r, info, maxEdge, img);
        bool ok = dr == PsdResult::Ok;
        if (!ok) {
            PsdLog("composite: %s", PsdResultName(dr));
            std::vector<uint8_t> jpeg;
            if (PsdDecoder::ReadJpegThumbnail(r, info, jpeg))
                ok = WicDecodeImage(jpeg.data(), jpeg.size(), maxEdge, info.jpegIsBgr, img);
            PsdLog("jpeg fallback: %s", ok ? "ok" : "failed");
        }
        if (!ok) return E_FAIL;

        HBITMAP hbm = CreateDibFromImage(img);
        if (!hbm) return E_OUTOFMEMORY;
        *phbmp = hbm;
        *pdwAlpha = img.hasAlpha ? WTSAT_ARGB : WTSAT_RGB;
        PsdLog("thumbnail %ux%u alpha=%d", img.width, img.height, img.hasAlpha ? 1 : 0);
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}
