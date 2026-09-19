#pragma once
#include "StreamReader.h"
#include <vector>

enum class PsdResult { Ok, NotPsd, Unsupported, Corrupt, NoComposite, IoError };
const char* PsdResultName(PsdResult r);

enum PsdColorMode : uint16_t {
    PSD_BITMAP = 0, PSD_GRAYSCALE = 1, PSD_INDEXED = 2, PSD_RGB = 3, PSD_CMYK = 4,
    PSD_MULTICHANNEL = 7, PSD_DUOTONE = 8, PSD_LAB = 9
};
const char* PsdColorModeName(uint16_t mode);

struct PsdInfo {
    uint16_t version = 0;              // 1 = PSD, 2 = PSB
    uint16_t channels = 0;
    uint32_t width = 0, height = 0;
    uint16_t depth = 0;                // 1, 8, 16, 32
    uint16_t colorMode = 0;
    uint16_t compression = 0;          // 0 raw, 1 RLE, 2/3 zip (never used for the composite by Photoshop)
    uint64_t imageDataOffset = 0;      // first byte after the compression word
    bool hasMergedTransparency = false;  // first extra channel holds the merged image's alpha
    bool hasVersionInfo = false;
    bool hasRealMergedData = true;     // false when saved without "Maximize Compatibility"
    // Embedded JPEG thumbnail (image resource 0x040C, or legacy 0x0409 in BGR order)
    uint64_t jpegOffset = 0;
    uint32_t jpegSize = 0;
    uint32_t jpegWidth = 0, jpegHeight = 0;
    bool jpegIsBgr = false;
    std::vector<uint8_t> palette;      // 768 bytes (R[256] G[256] B[256]) for indexed mode

    bool IsPsb() const { return version == 2; }
};

struct DecodedImage {
    uint32_t width = 0, height = 0;
    bool hasAlpha = false;
    std::vector<uint8_t> bgra;         // top-down, straight (non-premultiplied) alpha
};

class PsdDecoder {
public:
    static PsdResult ReadInfo(Reader& r, PsdInfo& info);
    // Decodes the merged (flattened) image, downsampled so that max(width, height) <= maxEdge.
    static PsdResult DecodeComposite(Reader& r, const PsdInfo& info, uint32_t maxEdge, DecodedImage& out);
    static bool ReadJpegThumbnail(Reader& r, const PsdInfo& info, std::vector<uint8_t>& jpeg);
};
