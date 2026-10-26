/*---------------------------------------------------------------------------------------------
 * guid-a-tron — SHA-1, from RFC 3174 section 6.1. See Sha1.h.
 *--------------------------------------------------------------------------------------------*/
#include "Sha1.h"

#include <string.h>

namespace guidatron {

namespace {
inline uint32_t RotateLeft(uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
}
} // namespace

//---------------------------------------------------------------------------------------------
void Sha1::Reset() {
    m_state[0] = 0x67452301;
    m_state[1] = 0xefcdab89;
    m_state[2] = 0x98badcfe;
    m_state[3] = 0x10325476;
    m_state[4] = 0xc3d2e1f0;
    m_bytes = 0;
    m_buffered = 0;
}

//---------------------------------------------------------------------------------------------
void Sha1::ProcessBlock(uint8_t const* block) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i)
        w[i] = ((uint32_t)block[4 * i] << 24) | ((uint32_t)block[4 * i + 1] << 16) |
               ((uint32_t)block[4 * i + 2] << 8) | (uint32_t)block[4 * i + 3];
    for (int i = 16; i < 80; ++i)
        w[i] = RotateLeft(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3], e = m_state[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5a827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdc;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6;
        }
        uint32_t temp = RotateLeft(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = RotateLeft(b, 30);
        b = a;
        a = temp;
    }
    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
    m_state[4] += e;
}

//---------------------------------------------------------------------------------------------
void Sha1::Add(void const* data, size_t size) {
    uint8_t const* at = (uint8_t const*)data;
    m_bytes += size;
    if (m_buffered > 0) {
        size_t take = BlockSize - m_buffered;
        if (take > size)
            take = size;
        memcpy(m_buffer + m_buffered, at, take);
        m_buffered += take;
        at += take;
        size -= take;
        if (m_buffered < BlockSize)
            return;
        ProcessBlock(m_buffer);
        m_buffered = 0;
    }
    while (size >= BlockSize) {
        ProcessBlock(at);
        at += BlockSize;
        size -= BlockSize;
    }
    if (size > 0) {
        memcpy(m_buffer, at, size);
        m_buffered = size;
    }
}

//---------------------------------------------------------------------------------------------
// The padding is applied to a copy of the state, so the digest can be taken more than once and
// `Add` can continue afterwards — which `ChangesetIdGenerator` relies on, since it takes the id
// after streaming a changeset it may still be reading more of.
//---------------------------------------------------------------------------------------------
void Sha1::GetHash(uint8_t out[HashBytes]) const {
    Sha1 copy(*this);
    uint64_t bits = copy.m_bytes * 8;

    uint8_t one = 0x80;
    copy.Add(&one, 1);
    uint8_t zero = 0x00;
    while (copy.m_buffered != BlockSize - 8)
        copy.Add(&zero, 1);
    uint8_t length[8];
    for (int i = 0; i < 8; ++i)
        length[i] = (uint8_t)(bits >> (56 - 8 * i));
    copy.Add(length, 8);

    for (int i = 0; i < 5; ++i) {
        out[4 * i] = (uint8_t)(copy.m_state[i] >> 24);
        out[4 * i + 1] = (uint8_t)(copy.m_state[i] >> 16);
        out[4 * i + 2] = (uint8_t)(copy.m_state[i] >> 8);
        out[4 * i + 3] = (uint8_t)copy.m_state[i];
    }
}

//---------------------------------------------------------------------------------------------
std::string Sha1::GetHashString() const {
    uint8_t digest[HashBytes];
    GetHash(digest);
    static char const* hex = "0123456789abcdef";
    std::string out;
    out.reserve(HashBytes * 2);
    for (int i = 0; i < HashBytes; ++i) {
        out.push_back(hex[digest[i] >> 4]);
        out.push_back(hex[digest[i] & 0x0f]);
    }
    return out;
}

} // namespace guidatron
