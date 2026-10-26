/*---------------------------------------------------------------------------------------------
 * guid-a-tron — SHA-1, because two things in this tool are defined in terms of it.
 *
 * A changeset's id is a SHA-1 over its parent's id, its prefix and every decompressed byte.
 *--------------------------------------------------------------------------------------------*/
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

namespace guidatron {

//! Incremental SHA-1. `Add` as often as you like, then take the digest once.
struct Sha1 {
    enum { HashBytes = 20, BlockSize = 64 };

    Sha1() { Reset(); }
    void Reset();
    void Add(void const* data, size_t size);

    //! The digest as 20 raw bytes.
    void GetHash(uint8_t out[HashBytes]) const;
    //! The digest as 40 lowercase hex characters, which is how a changeset id is written.
    std::string GetHashString() const;

private:
    void ProcessBlock(uint8_t const* block);

    uint32_t m_state[5];
    uint64_t m_bytes;              //!< total length, which the padding encodes
    uint8_t m_buffer[BlockSize];   //!< bytes not yet part of a full block
    size_t m_buffered;
};

} // namespace guidatron
