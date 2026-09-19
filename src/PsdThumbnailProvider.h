#pragma once
#include "psdthumb.h"
#include <propsys.h>
#include <thumbcache.h>

class PsdThumbnailProvider : public IInitializeWithStream, public IThumbnailProvider {
public:
    PsdThumbnailProvider();

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;

    // IInitializeWithStream
    IFACEMETHODIMP Initialize(IStream* pStream, DWORD grfMode) override;

    // IThumbnailProvider
    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* pdwAlpha) override;

private:
    ~PsdThumbnailProvider();
    LONG m_ref;
    IStream* m_stream;
};
