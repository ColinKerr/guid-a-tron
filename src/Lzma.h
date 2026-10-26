/*---------------------------------------------------------------------------------------------
 * guid-a-tron — LZMA2, which is what a changeset file's payload is compressed with.
 *
 * A thin layer over the vendored SDK (third_party/lzma), shaped to do exactly what reading and
 * writing a changeset container needs: push bytes in a page at a time while writing, pull them out
 * a page at a time while reading.
 *
 * **The stream has to be the one BeSQLite would have written**, because iTwin.js's native decoder
 * reads what this tool produces. Two things make that true, both taken from `BeLzma.cpp`:
 *
 *   - **The framing.** One byte of properties (`Lzma2Enc_WriteProperties`), then LZMA2 chunks,
 *     then a single `0x00` end marker. The decoder hands that first byte to `Lzma2Dec_Allocate`.
 *   - **The page discipline.** `LzmaEncoder::CompressNextPage` runs a whole `Lzma2Enc_Encode` over
 *     one page and drops the `0x00` the encoder puts at the end of it, so the file is a
 *     concatenation of per-page chunk sequences with one terminator at the very end. That costs
 *     some compression — each page resets the dictionary — but it is what the existing changesets
 *     are, so reproducing it is what lets a rewritten changeset be compared byte-for-byte with
 *     one BeSQLite's own writer produced from the same stream.
 *
 * The parameters are `LzmaEncoder::LzmaParams`' defaults, which is what `ChangesetFileWriter` uses:
 * a 16 MB dictionary, level 7, 8 "total threads" (inert — the SDK is compiled `_7ZIP_ST`), and no
 * random-access blocking.
 *--------------------------------------------------------------------------------------------*/
#pragma once

#include "Types.h"

#include <stdio.h>
#include <string>
#include <vector>

namespace guidatron {

//! Where a compressor puts its bytes. False on any short or failed write.
struct LzmaOutput {
    virtual ~LzmaOutput() {}
    virtual bool Write(void const* data, size_t size) = 0;
};

//! Where a decompressor gets its bytes. Returns how many it got; 0 means end of input.
struct LzmaInput {
    virtual ~LzmaInput() {}
    virtual size_t Read(void* data, size_t size) = 0;
};

struct FileLzmaOutput : LzmaOutput {
    ~FileLzmaOutput() override { Close(); }
    //! Creates or truncates. The caller has already decided it is allowed to.
    bool Create(std::string const& path);
    bool Write(void const* data, size_t size) override;
    void Close();
    bool IsOpen() const { return m_file != nullptr; }

private:
    FILE* m_file = nullptr;
};

struct FileLzmaInput : LzmaInput {
    ~FileLzmaInput() override { Close(); }
    bool Open(std::string const& path);
    size_t Read(void* data, size_t size) override;
    void Close();
    bool IsOpen() const { return m_file != nullptr; }

private:
    FILE* m_file = nullptr;
};

struct BufferLzmaOutput : LzmaOutput {
    explicit BufferLzmaOutput(std::vector<Byte>& into) : m_into(into) {}
    bool Write(void const* data, size_t size) override;

private:
    std::vector<Byte>& m_into;
};

struct BufferLzmaInput : LzmaInput {
    BufferLzmaInput(void const* data, size_t size) : m_data((Byte const*)data), m_size(size) {}
    size_t Read(void* data, size_t size) override;

private:
    Byte const* m_data;
    size_t m_size;
    size_t m_at = 0;
};

//=============================================================================================
//! Compresses into an LZMA2 stream, a page at a time. See the header comment for the framing.
//=============================================================================================
struct LzmaEncoder {
    LzmaEncoder();
    ~LzmaEncoder();
    LzmaEncoder(LzmaEncoder const&) = delete;
    LzmaEncoder& operator=(LzmaEncoder const&) = delete;

    //! Writes the one properties byte. `out` must outlive the encoder's use of it.
    bool StartCompress(LzmaOutput& out);
    bool CompressNextPage(void const* data, int size);
    //! Writes the end-of-stream marker. Safe to call twice; the second is a no-op.
    bool FinishCompress();

private:
    struct Impl;
    Impl* m_impl;
};

//=============================================================================================
//! Decompresses an LZMA2 stream, a page at a time.
//=============================================================================================
struct LzmaDecoder {
    LzmaDecoder();
    ~LzmaDecoder();
    LzmaDecoder(LzmaDecoder const&) = delete;
    LzmaDecoder& operator=(LzmaDecoder const&) = delete;

    //! Reads the properties byte and sizes the dictionary from it.
    bool StartDecompress(LzmaInput& in);
    /**
     * Up to `*size` bytes out. `*size` comes back as how many, and **0 means the stream is over**
     * — which is how the change-stream reader knows it has reached the end, since nothing in the
     * container records the decompressed length.
     */
    bool DecompressNextPage(void* data, int* size);
    void FinishDecompress();

private:
    struct Impl;
    Impl* m_impl;
};

} // namespace guidatron
