#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>

// {D18924F7-41A7-4C32-925D-0D5589BEF770}
static const CLSID CLSID_PsdThumbnailProvider =
    { 0xD18924F7, 0x41A7, 0x4C32, { 0x92, 0x5D, 0x0D, 0x55, 0x89, 0xBE, 0xF7, 0x70 } };

#define PSDTHUMB_CLSID_STRING   L"{D18924F7-41A7-4C32-925D-0D5589BEF770}"
#define PSDTHUMB_FRIENDLY_NAME  L"PSD Thumbnail Provider"
#define PSDTHUMB_VERSION        L"1.0.0"

// Largest edge we will ever decode to, whatever the shell asks for.
#define PSDTHUMB_MAX_EDGE 1024u

// Writes to %TEMP%\psd-thumbnails.log (and OutputDebugString) when PSDTHUMB_DEBUG=1.
void PsdLog(const char* fmt, ...);
