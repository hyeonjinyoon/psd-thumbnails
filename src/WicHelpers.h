#pragma once
#include "PsdDecoder.h"

// Decodes any WIC-supported image (we use it for the embedded JPEG thumbnail) into straight BGRA,
// scaled down so that max(width, height) <= maxEdge (0 = no scaling).
bool WicDecodeImage(const uint8_t* data, size_t len, uint32_t maxEdge, bool swapRB, DecodedImage& out);

bool WicSavePng(const DecodedImage& img, const wchar_t* path);

// 32bpp top-down DIB with premultiplied alpha, as the shell expects for WTSAT_ARGB.
HBITMAP CreateDibFromImage(const DecodedImage& img);
