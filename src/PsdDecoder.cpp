#include "PsdDecoder.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr uint32_t kMaxDimPsd = 30000;
constexpr uint32_t kMaxDimPsb = 300000;
constexpr uint32_t kMaxJpegBytes = 32u * 1024u * 1024u;

bool IsBlockSig(const uint8_t* s) {
    return s[0] == '8' && s[1] == 'B' && ((s[2] == 'I' && s[3] == 'M') || (s[2] == '6' && s[3] == '4'));
}

bool IsResourceSig(const uint8_t* s) {
    static const char* const kSigs[] = { "8BIM", "MeSa", "PHUT", "AgHg", "DCSR" };
    for (const char* k : kSigs)
        if (memcmp(s, k, 4) == 0) return true;
    return false;
}

// Additional-layer-info keys whose length field is 8 bytes in PSB files.
bool IsWideKey(const uint8_t* k) {
    static const char* const kKeys[] = { "LMsk", "Lr16", "Lr32", "Layr", "Mt16", "Mt32", "Mtrn",
                                         "Alph", "FMsk", "lnk2", "FEid", "FXid", "PxSD", "cinf" };
    for (const char* key : kKeys)
        if (memcmp(k, key, 4) == 0) return true;
    return false;
}

uint32_t BaseChannels(uint16_t mode) {
    switch (mode) {
    case PSD_BITMAP: case PSD_GRAYSCALE: case PSD_INDEXED: case PSD_DUOTONE: return 1;
    case PSD_RGB: case PSD_LAB: return 3;
    case PSD_CMYK: return 4;
    default: return 0;
    }
}

// PackBits. Lenient: zero-fills the output if the input runs short.
void UnpackBits(const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen) {
    size_t i = 0, o = 0;
    while (o < outLen && i < inLen) {
        const int8_t n = (int8_t)in[i++];
        if (n >= 0) {
            size_t cnt = (size_t)n + 1;
            if (cnt > inLen - i) cnt = inLen - i;
            if (cnt > outLen - o) cnt = outLen - o;
            memcpy(out + o, in + i, cnt);
            i += cnt;
            o += cnt;
        } else if (n != -128) {
            size_t cnt = (size_t)(1 - (int)n);
            if (i >= inLen) break;
            const uint8_t b = in[i++];
            if (cnt > outLen - o) cnt = outLen - o;
            memset(out + o, b, cnt);
            o += cnt;
        }
    }
    if (o < outLen) memset(out + o, 0, outLen - o);
}

float SrgbEncode(float v) {
    if (v <= 0.f) return 0.f;
    if (v >= 1.f) return 1.f;
    return v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
}

uint8_t ToByte(float v) {
    if (v <= 0.f) return 0;
    if (v >= 255.f) return 255;
    return (uint8_t)(v + 0.5f);
}

// CIE L*a*b* (D50) -> sRGB, all outputs 0..255.
void LabToRgb(float L, float a, float b, float& R, float& G, float& B) {
    const float fy = (L + 16.f) / 116.f;
    const float fx = fy + a / 500.f;
    const float fz = fy - b / 200.f;
    auto finv = [](float t) { const float t3 = t * t * t; return t3 > 0.008856f ? t3 : (t - 16.f / 116.f) / 7.787f; };
    const float X = 0.9642f * finv(fx), Y = finv(fy), Z = 0.8249f * finv(fz);
    const float rl =  3.1339f * X - 1.6170f * Y - 0.4906f * Z;
    const float gl = -0.9785f * X + 1.9160f * Y + 0.0333f * Z;
    const float bl =  0.0720f * X - 0.2290f * Y + 1.4057f * Z;
    R = SrgbEncode(rl) * 255.f;
    G = SrgbEncode(gl) * 255.f;
    B = SrgbEncode(bl) * 255.f;
}

float ReadBeF32(const uint8_t* p) {
    const uint32_t u = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    float f;
    memcpy(&f, &u, 4);
    return f;
}

// Adds, for every output column tx, the sum of source samples x in [colStart[tx], colStart[tx + 1])
// to acc[tx * stride]. Units: 0..255 for 8/16-bit and bitmap, linear light for 32-bit.
void AccumulateRow(const uint8_t* raw, uint16_t depth, const uint32_t* colStart, uint32_t tw, float* acc, uint32_t stride) {
    for (uint32_t tx = 0; tx < tw; ++tx, acc += stride) {
        const uint32_t x0 = colStart[tx], x1 = colStart[tx + 1];
        switch (depth) {
        case 8: {
            uint32_t s = 0;
            for (uint32_t x = x0; x < x1; ++x) s += raw[x];
            *acc += (float)s;
            break;
        }
        case 16: {
            uint64_t s = 0;
            for (uint32_t x = x0; x < x1; ++x) s += (uint32_t)((raw[2 * x] << 8) | raw[2 * x + 1]);
            *acc += (float)s * (255.f / 65535.f);
            break;
        }
        case 32: {
            float s = 0.f;
            for (uint32_t x = x0; x < x1; ++x) {
                const float f = ReadBeF32(raw + 4 * x);
                if (std::isfinite(f)) s += f;
            }
            *acc += s;
            break;
        }
        case 1: {
            uint32_t white = 0;
            for (uint32_t x = x0; x < x1; ++x) white += (raw[x >> 3] & (0x80 >> (x & 7))) ? 0u : 1u;  // 1 = black
            *acc += (float)white * 255.f;
            break;
        }
        default:
            break;
        }
    }
}

// 16/32-bit documents keep their layer records inside a 'Lr16'/'Lr32' block; the layer
// count sign there tells us whether the merged image carries transparency.
void ParseAdditionalLayerInfo(Reader& r, bool psb, uint64_t pos, uint64_t end, PsdInfo& info) {
    if (!r.Seek(pos)) return;
    uint32_t globalMaskLen;
    if (!r.ReadU32(globalMaskLen) || !r.Skip(globalMaskLen)) return;
    for (int guard = 0; guard < 512 && r.Tell() + 12 <= end; ++guard) {
        uint8_t sig[4];
        bool found = false;
        for (int pad = 0; pad < 4; ++pad) {  // writers pad blocks to 2 or 4 bytes
            if (!r.Peek(sig, 4)) return;
            if (IsBlockSig(sig)) { found = true; break; }
            if (!r.Skip(1)) return;
        }
        if (!found) return;
        uint8_t key[4];
        if (!r.Skip(4) || !r.Read(key, 4)) return;
        uint64_t len;
        if (!r.ReadLen(psb && IsWideKey(key), len)) return;
        const uint64_t dataStart = r.Tell();
        if (len > end - dataStart) return;
        if (memcmp(key, "Lr16", 4) == 0 || memcmp(key, "Lr32", 4) == 0) {
            uint64_t innerLen;
            int16_t count;
            if (r.ReadLen(psb, innerLen) && innerLen >= 2 && r.ReadI16(count)) info.hasMergedTransparency = count < 0;
            return;
        }
        if (!r.Seek(dataStart + len)) return;
    }
}

}  // namespace

const char* PsdResultName(PsdResult r) {
    switch (r) {
    case PsdResult::Ok: return "ok";
    case PsdResult::NotPsd: return "not a PSD file";
    case PsdResult::Unsupported: return "unsupported";
    case PsdResult::Corrupt: return "corrupt";
    case PsdResult::NoComposite: return "no composite image (saved without Maximize Compatibility)";
    case PsdResult::IoError: return "read error";
    }
    return "?";
}

const char* PsdColorModeName(uint16_t mode) {
    switch (mode) {
    case PSD_BITMAP: return "Bitmap";
    case PSD_GRAYSCALE: return "Grayscale";
    case PSD_INDEXED: return "Indexed";
    case PSD_RGB: return "RGB";
    case PSD_CMYK: return "CMYK";
    case PSD_MULTICHANNEL: return "Multichannel";
    case PSD_DUOTONE: return "Duotone";
    case PSD_LAB: return "Lab";
    }
    return "Unknown";
}

PsdResult PsdDecoder::ReadInfo(Reader& r, PsdInfo& info) {
    info = PsdInfo();
    if (!r.Seek(0)) return PsdResult::IoError;
    uint8_t sig[4];
    if (!r.Read(sig, 4)) return PsdResult::IoError;
    if (memcmp(sig, "8BPS", 4) != 0) return PsdResult::NotPsd;
    if (!r.ReadU16(info.version)) return PsdResult::IoError;
    if (info.version != 1 && info.version != 2) return PsdResult::Unsupported;
    const bool psb = info.IsPsb();
    if (!r.Skip(6)) return PsdResult::IoError;
    if (!r.ReadU16(info.channels) || !r.ReadU32(info.height) || !r.ReadU32(info.width) ||
        !r.ReadU16(info.depth) || !r.ReadU16(info.colorMode))
        return PsdResult::IoError;

    const uint32_t maxDim = psb ? kMaxDimPsb : kMaxDimPsd;
    if (info.channels < 1 || info.channels > 56) return PsdResult::Corrupt;
    if (info.width < 1 || info.width > maxDim || info.height < 1 || info.height > maxDim) return PsdResult::Corrupt;
    if (info.depth != 1 && info.depth != 8 && info.depth != 16 && info.depth != 32) return PsdResult::Corrupt;
    switch (info.colorMode) {
    case PSD_BITMAP: case PSD_GRAYSCALE: case PSD_INDEXED: case PSD_RGB:
    case PSD_CMYK: case PSD_MULTICHANNEL: case PSD_DUOTONE: case PSD_LAB: break;
    default: return PsdResult::Corrupt;
    }
    const uint64_t fileSize = r.Size();
    const bool sizeKnown = fileSize != Reader::kUnknownSize;

    // Color mode data (palette for indexed images)
    uint32_t cmLen;
    if (!r.ReadU32(cmLen)) return PsdResult::IoError;
    if (sizeKnown && cmLen > fileSize) return PsdResult::Corrupt;
    {
        const uint64_t end = r.Tell() + cmLen;
        if (info.colorMode == PSD_INDEXED && cmLen >= 768) {
            info.palette.resize(768);
            if (!r.Read(info.palette.data(), 768)) return PsdResult::IoError;
        }
        if (!r.Seek(end)) return PsdResult::Corrupt;
    }

    // Image resources: we only care about the JPEG thumbnail and the version-info flag.
    uint32_t irLen;
    if (!r.ReadU32(irLen)) return PsdResult::IoError;
    if (sizeKnown && irLen > fileSize) return PsdResult::Corrupt;
    {
        const uint64_t end = r.Tell() + irLen;
        while (r.Tell() + 12 <= end) {
            uint8_t rs[4];
            uint16_t id;
            uint8_t nameLen;
            if (!r.Read(rs, 4) || !IsResourceSig(rs)) break;
            if (!r.ReadU16(id) || !r.ReadU8(nameLen)) break;
            if (!r.Skip(nameLen + (((nameLen + 1) & 1) ? 1 : 0))) break;
            uint32_t size;
            if (!r.ReadU32(size)) break;
            const uint64_t dataStart = r.Tell();
            if (size > end - dataStart) break;
            if ((id == 0x040C || id == 0x0409) && size > 28 && (info.jpegSize == 0 || id == 0x040C)) {
                uint32_t format, w, h;
                if (r.ReadU32(format) && r.ReadU32(w) && r.ReadU32(h) && format == 1 && w > 0 && h > 0) {
                    info.jpegOffset = dataStart + 28;
                    info.jpegSize = size - 28;
                    info.jpegWidth = w;
                    info.jpegHeight = h;
                    info.jpegIsBgr = (id == 0x0409);
                }
            } else if (id == 0x0421 && size >= 5) {
                uint32_t ver;
                uint8_t real;
                if (r.ReadU32(ver) && r.ReadU8(real)) {
                    info.hasVersionInfo = true;
                    info.hasRealMergedData = real != 0;
                }
            }
            if (!r.Seek(dataStart + size + (size & 1))) break;
        }
        if (!r.Seek(end)) return PsdResult::Corrupt;
    }

    // Layer and mask information: only the sign of the layer count matters to us.
    uint64_t lmLen;
    if (!r.ReadLen(psb, lmLen)) return PsdResult::IoError;
    if (sizeKnown && lmLen > fileSize) return PsdResult::Corrupt;
    {
        const uint64_t lmStart = r.Tell();
        const uint64_t lmEnd = lmStart + lmLen;
        if (lmLen >= (psb ? 8u : 4u)) {
            uint64_t liLen;
            if (!r.ReadLen(psb, liLen)) return PsdResult::IoError;
            const uint64_t liStart = r.Tell();
            if (liLen >= 2 && liLen <= lmEnd - liStart) {
                int16_t count;
                if (r.ReadI16(count)) info.hasMergedTransparency = count < 0;
            } else if (liLen == 0) {
                ParseAdditionalLayerInfo(r, psb, liStart, lmEnd, info);
            }
        }
        if (!r.Seek(lmEnd)) return PsdResult::Corrupt;
    }

    if (!r.ReadU16(info.compression)) return PsdResult::IoError;
    info.imageDataOffset = r.Tell();
    return PsdResult::Ok;
}

PsdResult PsdDecoder::DecodeComposite(Reader& r, const PsdInfo& info, uint32_t maxEdge, DecodedImage& out) {
    if (!info.hasRealMergedData) return PsdResult::NoComposite;
    if (info.compression > 1) return PsdResult::Unsupported;
    if ((info.depth == 1) != (info.colorMode == PSD_BITMAP)) return PsdResult::Unsupported;
    if (info.colorMode == PSD_INDEXED && (info.depth != 8 || info.palette.size() < 768)) return PsdResult::Unsupported;
    if (maxEdge == 0) maxEdge = 1;
    if (maxEdge > PSDTHUMB_MAX_EDGE) maxEdge = PSDTHUMB_MAX_EDGE;

    const uint32_t W = info.width, H = info.height;
    const bool multichannel = info.colorMode == PSD_MULTICHANNEL;
    const bool psb = info.IsPsb();
    uint32_t colorCh;
    if (multichannel) {
        colorCh = std::min<uint32_t>(info.channels, 4);
    } else {
        colorCh = BaseChannels(info.colorMode);
        if (colorCh == 0) return PsdResult::Unsupported;
        if (info.channels < colorCh) return PsdResult::Corrupt;
    }
    const bool hasAlpha = !multichannel && info.hasMergedTransparency && info.channels > colorCh &&
                          info.colorMode != PSD_BITMAP && info.colorMode != PSD_INDEXED;
    const uint32_t chRead = colorCh + (hasAlpha ? 1 : 0);
    const uint32_t alphaCh = hasAlpha ? colorCh : 0;

    const bool bitmap = info.depth == 1;
    const uint64_t rowBytes = bitmap ? ((uint64_t)W + 7) / 8 : (uint64_t)W * (info.depth / 8);
    const uint64_t nRows = (uint64_t)chRead * H;

    // Locate every row we might need. RLE rows are found through the byte-count table.
    std::vector<uint64_t> rleOffsets;
    uint64_t dataStart = info.imageDataOffset;
    if (info.compression == 1) {
        const uint64_t entrySize = psb ? 4 : 2;
        std::vector<uint8_t> tbl((size_t)(nRows * entrySize));
        if (!r.Seek(info.imageDataOffset) || !r.Read(tbl.data(), tbl.size())) return PsdResult::Corrupt;
        dataStart = info.imageDataOffset + (uint64_t)info.channels * H * entrySize;
        rleOffsets.resize((size_t)nRows + 1);
        uint64_t off = dataStart;
        for (uint64_t i = 0; i < nRows; ++i) {
            rleOffsets[(size_t)i] = off;
            const uint8_t* p = tbl.data() + i * entrySize;
            off += psb ? (((uint64_t)p[0] << 24) | ((uint64_t)p[1] << 16) | ((uint64_t)p[2] << 8) | p[3])
                       : (((uint64_t)p[0] << 8) | p[1]);
        }
        rleOffsets[(size_t)nRows] = off;
        if (r.Size() != Reader::kUnknownSize && off > r.Size()) return PsdResult::Corrupt;
    } else if (r.Size() != Reader::kUnknownSize && dataStart + rowBytes * nRows > r.Size()) {
        return PsdResult::Corrupt;
    }

    // Output size and sampling grid.
    const uint32_t maxSrc = std::max(W, H);
    uint32_t tw = W, th = H;
    if (maxSrc > maxEdge) {
        const double s = (double)maxEdge / maxSrc;
        tw = std::clamp<uint32_t>((uint32_t)std::lround(W * s), 1, maxEdge);
        th = std::clamp<uint32_t>((uint32_t)std::lround(H * s), 1, maxEdge);
    }
    // Sample about twice as many source rows as output rows; every output row gets >= 1 sample.
    const uint32_t rowStep = std::max<uint32_t>(1, H / (th * 2));

    // Read-ahead only pays off when the next sampled row lands in the same buffer;
    // otherwise every refill drags in rows we are about to skip.
    const uint64_t avgRowBytes = info.compression == 1 ? (rleOffsets[(size_t)nRows] - rleOffsets[0]) / nRows : rowBytes;
    if (avgRowBytes * rowStep > r.BufferSize()) r.SetReadAhead(0);

    // Output column tx averages source columns [colStart[tx], colStart[tx + 1]).
    std::vector<uint32_t> colStart(tw + 1), rowCount(th, 0);
    for (uint32_t tx = 0; tx <= tw; ++tx) colStart[tx] = (uint32_t)(((uint64_t)tx * W + tw - 1) / tw);

    const bool indexed = info.colorMode == PSD_INDEXED;
    const uint32_t nAcc = indexed ? 3 : chRead;
    std::vector<float> acc((size_t)tw * th * nAcc, 0.f);
    std::vector<uint8_t> raw((size_t)rowBytes), packed;
    const uint64_t maxPacked = rowBytes * 2 + 64;

    for (uint32_t c = 0; c < chRead; ++c) {
        for (uint32_t y = 0; y < H; y += rowStep) {
            if (info.compression == 1) {
                const size_t idx = (size_t)((uint64_t)c * H + y);
                const uint64_t a = rleOffsets[idx], b = rleOffsets[idx + 1];
                if (b < a || b - a > maxPacked) return PsdResult::Corrupt;
                packed.resize((size_t)(b - a));
                if (!r.Seek(a) || !r.Read(packed.data(), packed.size())) return PsdResult::IoError;
                UnpackBits(packed.data(), packed.size(), raw.data(), raw.size());
            } else {
                const uint64_t off = dataStart + ((uint64_t)c * H + y) * rowBytes;
                if (!r.Seek(off) || !r.Read(raw.data(), raw.size())) return PsdResult::IoError;
            }
            const uint32_t ty = (uint32_t)(((uint64_t)y * th) / H);
            if (c == 0) rowCount[ty]++;
            float* accRow = acc.data() + (size_t)ty * tw * nAcc;
            if (indexed) {
                const uint8_t* pal = info.palette.data();
                for (uint32_t tx = 0; tx < tw; ++tx) {
                    uint32_t s0 = 0, s1 = 0, s2 = 0;
                    for (uint32_t x = colStart[tx]; x < colStart[tx + 1]; ++x) {
                        const uint8_t i = raw[x];
                        s0 += pal[i];
                        s1 += pal[256 + i];
                        s2 += pal[512 + i];
                    }
                    float* a = accRow + (size_t)tx * 3;
                    a[0] += (float)s0;
                    a[1] += (float)s1;
                    a[2] += (float)s2;
                }
            } else {
                AccumulateRow(raw.data(), info.depth, colStart.data(), tw, accRow + c, nAcc);
            }
        }
    }

    // Average, then convert each output pixel to sRGB.
    out.width = tw;
    out.height = th;
    out.hasAlpha = hasAlpha;
    out.bgra.assign((size_t)tw * th * 4, 0);
    const bool isFloat = info.depth == 32;
    std::vector<float> v(nAcc);
    uint8_t* dst = out.bgra.data();
    for (uint32_t ty = 0; ty < th; ++ty) {
        const float* accRow = acc.data() + (size_t)ty * tw * nAcc;
        for (uint32_t tx = 0; tx < tw; ++tx, dst += 4) {
            const float n = (float)rowCount[ty] * (float)(colStart[tx + 1] - colStart[tx]);
            const float inv = n > 0.f ? 1.f / n : 0.f;
            for (uint32_t k = 0; k < nAcc; ++k) v[k] = accRow[(size_t)tx * nAcc + k] * inv;
            if (isFloat) {
                for (uint32_t k = 0; k < colorCh; ++k) v[k] = SrgbEncode(v[k]) * 255.f;
                if (hasAlpha) v[alphaCh] = std::clamp(v[alphaCh], 0.f, 1.f) * 255.f;
            }
            float R = 0.f, G = 0.f, B = 0.f;
            switch (info.colorMode) {
            case PSD_BITMAP: case PSD_GRAYSCALE: case PSD_DUOTONE:
                R = G = B = v[0];
                break;
            case PSD_INDEXED: case PSD_RGB:
                R = v[0]; G = v[1]; B = v[2];
                break;
            case PSD_CMYK: {  // stored inverted: 255 = no ink
                const float k = v[3] / 255.f;
                R = v[0] * k; G = v[1] * k; B = v[2] * k;
                break;
            }
            case PSD_MULTICHANNEL:  // spot channels, stored inverted like CMYK
                if (colorCh == 1) { R = G = B = v[0]; }
                else if (colorCh == 2) { R = v[0]; G = v[1]; B = 255.f; }
                else { const float k = colorCh >= 4 ? v[3] / 255.f : 1.f; R = v[0] * k; G = v[1] * k; B = v[2] * k; }
                break;
            case PSD_LAB:
                LabToRgb(v[0] * (100.f / 255.f), v[1] - 128.f, v[2] - 128.f, R, G, B);
                break;
            default:
                break;
            }
            dst[0] = ToByte(B);
            dst[1] = ToByte(G);
            dst[2] = ToByte(R);
            dst[3] = hasAlpha ? ToByte(v[alphaCh]) : 255;
        }
    }
    return PsdResult::Ok;
}

bool PsdDecoder::ReadJpegThumbnail(Reader& r, const PsdInfo& info, std::vector<uint8_t>& jpeg) {
    if (info.jpegSize == 0 || info.jpegSize > kMaxJpegBytes) return false;
    jpeg.resize(info.jpegSize);
    return r.Seek(info.jpegOffset) && r.Read(jpeg.data(), jpeg.size());
}
