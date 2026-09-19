#include "StreamReader.h"
#include <cstring>

Reader::Reader(IStream* stream, size_t bufferSize) : m_stream(stream), m_buf(bufferSize ? bufferSize : 4096) {
    if (m_stream) {
        m_stream->AddRef();
        STATSTG st = {};
        if (SUCCEEDED(m_stream->Stat(&st, STATFLAG_NONAME))) m_size = st.cbSize.QuadPart;
    }
}

Reader::~Reader() {
    if (m_stream) m_stream->Release();
}

bool Reader::Seek(uint64_t pos) {
    if (m_size != kUnknownSize && pos > m_size) return false;
    m_pos = pos;
    return true;
}

bool Reader::Skip(uint64_t n) {
    if (n > ~0ull - m_pos) return false;
    return Seek(m_pos + n);
}

bool Reader::Fill(uint64_t pos) {
    if (!m_stream) return false;
    if (!m_streamPosKnown || m_streamPos != pos) {
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)pos;
        ULARGE_INTEGER np = {};
        if (FAILED(m_stream->Seek(li, STREAM_SEEK_SET, &np))) {
            m_streamPosKnown = false;
            return false;
        }
        m_streamPos = np.QuadPart;
        m_streamPosKnown = true;
        if (m_streamPos != pos) return false;
    }
    ULONG got = 0;
    const HRESULT hr = m_stream->Read(m_buf.data(), (ULONG)m_buf.size(), &got);
    if (FAILED(hr) || got == 0) {
        m_streamPosKnown = false;
        m_bufLen = 0;
        return false;
    }
    m_bufStart = pos;
    m_bufLen = got;
    m_streamPos = pos + got;
    return true;
}

bool Reader::Read(void* dst, size_t n) {
    uint8_t* out = static_cast<uint8_t*>(dst);
    while (n > 0) {
        if (m_pos >= m_bufStart && m_pos < m_bufStart + m_bufLen) {
            const size_t off = (size_t)(m_pos - m_bufStart);
            const size_t avail = m_bufLen - off;
            const size_t take = avail < n ? avail : n;
            memcpy(out, m_buf.data() + off, take);
            out += take;
            n -= take;
            m_pos += take;
        } else if (!Fill(m_pos)) {
            return false;
        }
    }
    return true;
}

bool Reader::Peek(void* dst, size_t n) {
    const uint64_t p = m_pos;
    const bool ok = Read(dst, n);
    m_pos = p;
    return ok;
}

bool Reader::ReadU8(uint8_t& v) { return Read(&v, 1); }

bool Reader::ReadU16(uint16_t& v) {
    uint8_t b[2];
    if (!Read(b, 2)) return false;
    v = (uint16_t)((b[0] << 8) | b[1]);
    return true;
}

bool Reader::ReadU32(uint32_t& v) {
    uint8_t b[4];
    if (!Read(b, 4)) return false;
    v = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    return true;
}

bool Reader::ReadU64(uint64_t& v) {
    uint8_t b[8];
    if (!Read(b, 8)) return false;
    v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | b[i];
    return true;
}

bool Reader::ReadI16(int16_t& v) {
    uint16_t u;
    if (!ReadU16(u)) return false;
    v = (int16_t)u;
    return true;
}

bool Reader::ReadLen(bool wide, uint64_t& v) {
    if (wide) return ReadU64(v);
    uint32_t u;
    if (!ReadU32(u)) return false;
    v = u;
    return true;
}
