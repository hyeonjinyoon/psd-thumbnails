#pragma once
#include "psdthumb.h"
#include <objidl.h>
#include <vector>

// Buffered big-endian reader over an IStream.
// Seeks are lazy: seeking inside the current buffer costs nothing, so reading
// every N-th row of a large file degrades gracefully into sequential I/O.
class Reader {
public:
    static const uint64_t kUnknownSize = ~0ull;

    explicit Reader(IStream* stream, size_t bufferSize = 512 * 1024);
    ~Reader();
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    uint64_t Size() const { return m_size; }
    uint64_t Tell() const { return m_pos; }
    bool Seek(uint64_t pos);
    bool Skip(uint64_t n);
    bool Read(void* dst, size_t n);   // exact read; false on EOF / error
    bool Peek(void* dst, size_t n);   // read without advancing

    bool ReadU8(uint8_t& v);
    bool ReadU16(uint16_t& v);
    bool ReadU32(uint32_t& v);
    bool ReadU64(uint64_t& v);
    bool ReadI16(int16_t& v);
    bool ReadLen(bool wide, uint64_t& v);  // u32 (PSD) or u64 (PSB)

    // Caps how far a buffer refill reads past the requested bytes (0 = only what was asked for).
    void SetReadAhead(size_t n) { m_readAhead = n; }
    size_t BufferSize() const { return m_buf.size(); }

private:
    bool Fill(uint64_t pos, size_t want);

    IStream* m_stream;
    std::vector<uint8_t> m_buf;
    uint64_t m_bufStart = 0;
    size_t m_bufLen = 0;
    uint64_t m_pos = 0;
    uint64_t m_streamPos = 0;
    bool m_streamPosKnown = false;
    uint64_t m_size = kUnknownSize;
    size_t m_readAhead = ~(size_t)0;
};
