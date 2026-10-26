/*---------------------------------------------------------------------------------------------
 * guid-a-tron — LZMA2. See Lzma.h for why the framing and the page discipline are what they are.
 *--------------------------------------------------------------------------------------------*/
#include "Lzma.h"

#include "Files.h"

#include "Lzma2Dec.h"
#include "Lzma2Enc.h"

#include <string.h>

namespace guidatron {

namespace {

constexpr int DECODE_INPUT_BUFFER_SIZE = 32 * 1024; // BeLzma.cpp's own figure

void* AllocFor7z(void*, size_t size) { return malloc(size); }
void FreeFor7z(void*, void* address) { free(address); }

//! The SDK pulls from an ISeqInStream; this one hands it a single page and then reports EOF.
struct PageInStream : ::ISeqInStream {
    Byte const* data = nullptr;
    size_t size = 0;
    size_t at = 0;

    PageInStream() { Read = &PageInStream::ReadStatic; }
    void Set(void const* from, size_t bytes) {
        data = (Byte const*)from;
        size = bytes;
        at = 0;
    }
    static SRes ReadStatic(void* p, void* buf, size_t* bufSize) {
        PageInStream* self = (PageInStream*)p;
        size_t take = self->size - self->at;
        if (take > *bufSize)
            take = *bufSize;
        memcpy(buf, self->data + self->at, take);
        self->at += take;
        *bufSize = take;
        return SZ_OK;
    }
};

//! The SDK pushes to an ISeqOutStream; this one collects into a vector so the trailing end-of-
//! stream byte can be dropped before the page is written out.
struct VectorOutStream : ::ISeqOutStream {
    std::vector<Byte> buffer;

    VectorOutStream() { Write = &VectorOutStream::WriteStatic; }
    static size_t WriteStatic(void* p, void const* buf, size_t size) {
        VectorOutStream* self = (VectorOutStream*)p;
        Byte const* from = (Byte const*)buf;
        self->buffer.insert(self->buffer.end(), from, from + size);
        return size;
    }
};

//! The SDK reads through an ISeqInStream here too, so the decoder can pull from any LzmaInput.
struct AdapterInStream : ::ISeqInStream {
    LzmaInput* source = nullptr;

    AdapterInStream() { Read = &AdapterInStream::ReadStatic; }
    static SRes ReadStatic(void* p, void* buf, size_t* bufSize) {
        AdapterInStream* self = (AdapterInStream*)p;
        *bufSize = self->source->Read(buf, *bufSize);
        return SZ_OK;
    }
};

} // namespace

//=============================================================================================
// The streams
//=============================================================================================

bool FileLzmaOutput::Create(std::string const& path) {
    Close();
    CreateDirectories(ParentDir(path));
    m_file = fopen(path.c_str(), "wb");
    return m_file != nullptr;
}
bool FileLzmaOutput::Write(void const* data, size_t size) {
    if (m_file == nullptr)
        return false;
    return size == 0 || fwrite(data, 1, size, m_file) == size;
}
void FileLzmaOutput::Close() {
    if (m_file != nullptr)
        fclose(m_file);
    m_file = nullptr;
}

bool FileLzmaInput::Open(std::string const& path) {
    Close();
    m_file = fopen(path.c_str(), "rb");
    return m_file != nullptr;
}
size_t FileLzmaInput::Read(void* data, size_t size) {
    return m_file == nullptr ? 0 : fread(data, 1, size, m_file);
}
void FileLzmaInput::Close() {
    if (m_file != nullptr)
        fclose(m_file);
    m_file = nullptr;
}

bool BufferLzmaOutput::Write(void const* data, size_t size) {
    Byte const* from = (Byte const*)data;
    m_into.insert(m_into.end(), from, from + size);
    return true;
}

size_t BufferLzmaInput::Read(void* data, size_t size) {
    size_t take = m_size - m_at;
    if (take > size)
        take = size;
    memcpy(data, m_data + m_at, take);
    m_at += take;
    return take;
}

//=============================================================================================
// LzmaEncoder
//=============================================================================================

struct LzmaEncoder::Impl {
    ::ISzAlloc alloc;
    CLzma2EncProps props;
    CLzma2EncHandle handle = nullptr;
    LzmaOutput* out = nullptr;
    bool finished = false;
    PageInStream in;
    VectorOutStream collected;

    Impl() {
        alloc.Alloc = AllocFor7z;
        alloc.Free = FreeFor7z;
        // `LzmaEncoder::LzmaParams(1 << 24, false, 7, 8)`, which is `ChangesetFileWriter`'s
        // default, normalized the way BeLzma normalizes it: once when the parameters are built and
        // again when the encoder is constructed from them. Normalize is idempotent, so the second
        // call changes nothing — it is here because the path it reproduces makes two.
        Lzma2EncProps_Init(&props);
        props.lzmaProps.level = 7;
        props.lzmaProps.dictSize = 1 << 24; // 16 MB, already a legal dictionary size
        props.numTotalThreads = 8;
        Lzma2EncProps_Normalize(&props);
        Lzma2EncProps_Normalize(&props);
    }

    ~Impl() { Destroy(); }

    void Destroy() {
        if (handle != nullptr)
            Lzma2Enc_Destroy(handle);
        handle = nullptr;
    }
};

LzmaEncoder::LzmaEncoder() : m_impl(new Impl()) {}
LzmaEncoder::~LzmaEncoder() { delete m_impl; }

//---------------------------------------------------------------------------------------------
bool LzmaEncoder::StartCompress(LzmaOutput& out) {
    if (m_impl->handle != nullptr)
        return false;
    m_impl->handle = Lzma2Enc_Create(&m_impl->alloc, &m_impl->alloc);
    if (m_impl->handle == nullptr)
        return false;
    if (SZ_OK != Lzma2Enc_SetProps(m_impl->handle, &m_impl->props)) {
        m_impl->Destroy();
        return false;
    }
    m_impl->out = &out;
    m_impl->finished = false;
    ::Byte properties = Lzma2Enc_WriteProperties(m_impl->handle);
    return out.Write(&properties, 1);
}

//---------------------------------------------------------------------------------------------
// One `Lzma2Enc_Encode` per page, with the end-of-stream byte it appends dropped — see Lzma.h.
//---------------------------------------------------------------------------------------------
bool LzmaEncoder::CompressNextPage(void const* data, int size) {
    if (m_impl->handle == nullptr || m_impl->out == nullptr || size <= 0)
        return size == 0; // an empty page is nothing to compress, not a failure
    m_impl->collected.buffer.clear();
    m_impl->in.Set(data, (size_t)size);
    if (SZ_OK != Lzma2Enc_Encode(m_impl->handle, &m_impl->collected, &m_impl->in, nullptr))
        return false;
    std::vector<Byte>& chunks = m_impl->collected.buffer;
    if (chunks.empty() || chunks.back() != 0)
        return false; // the encoder always ends a stream with the marker; without it, something is wrong
    return m_impl->out->Write(chunks.data(), chunks.size() - 1);
}

//---------------------------------------------------------------------------------------------
bool LzmaEncoder::FinishCompress() {
    if (m_impl->handle == nullptr || m_impl->finished)
        return true;
    m_impl->finished = true;
    ::Byte endOfStream = 0;
    bool ok = m_impl->out != nullptr && m_impl->out->Write(&endOfStream, 1);
    m_impl->Destroy();
    m_impl->out = nullptr;
    return ok;
}

//=============================================================================================
// LzmaDecoder
//=============================================================================================

struct LzmaDecoder::Impl {
    ::ISzAlloc alloc;
    CLzma2Dec state;
    bool allocated = false;
    AdapterInStream in;
    ::Byte inBuffer[DECODE_INPUT_BUFFER_SIZE];
    size_t inSize = 0;
    size_t inPos = 0;

    Impl() {
        alloc.Alloc = AllocFor7z;
        alloc.Free = FreeFor7z;
        memset(&state, 0, sizeof(state));
    }
    ~Impl() { Free(); }
    void Free() {
        if (allocated)
            Lzma2Dec_Free(&state, &alloc);
        allocated = false;
    }
};

LzmaDecoder::LzmaDecoder() : m_impl(new Impl()) {}
LzmaDecoder::~LzmaDecoder() { delete m_impl; }

//---------------------------------------------------------------------------------------------
bool LzmaDecoder::StartDecompress(LzmaInput& in) {
    if (m_impl->allocated)
        return false;
    m_impl->in.source = &in;
    ::Byte properties = 0;
    if (1 != in.Read(&properties, 1))
        return false;
    Lzma2Dec_Construct(&m_impl->state);
    if (SZ_OK != Lzma2Dec_Allocate(&m_impl->state, properties, &m_impl->alloc))
        return false;
    m_impl->allocated = true;
    Lzma2Dec_Init(&m_impl->state);
    m_impl->inSize = m_impl->inPos = 0;
    return true;
}

//---------------------------------------------------------------------------------------------
// BeLzma's loop: refill when the input buffer is spent, decode, and keep going until something
// comes out. An input read of 0 bytes is the end of the stream — the container records no
// decompressed length, so this is the only end marker there is.
//---------------------------------------------------------------------------------------------
bool LzmaDecoder::DecompressNextPage(void* data, int* size) {
    if (!m_impl->allocated || size == nullptr)
        return false;
    for (;;) {
        if (m_impl->inPos == m_impl->inSize) {
            m_impl->inSize = m_impl->in.source->Read(m_impl->inBuffer, sizeof(m_impl->inBuffer));
            m_impl->inPos = 0;
            if (m_impl->inSize == 0) {
                *size = 0;
                return true;
            }
        }
        SizeT inProcessed = m_impl->inSize - m_impl->inPos;
        SizeT outProcessed = (SizeT)*size;
        ELzmaStatus status;
        SRes result = Lzma2Dec_DecodeToBuf(&m_impl->state, (::Byte*)data, &outProcessed,
                                           m_impl->inBuffer + m_impl->inPos, &inProcessed,
                                           LZMA_FINISH_ANY, &status);
        m_impl->inPos += inProcessed;
        if (SZ_OK != result)
            return false;
        if (outProcessed > 0) {
            *size = (int)outProcessed;
            return true;
        }
        if (inProcessed == 0)
            return false; // no progress either way: the stream is truncated or corrupt
    }
}

//---------------------------------------------------------------------------------------------
void LzmaDecoder::FinishDecompress() {
    m_impl->Free();
    m_impl->in.source = nullptr;
}

} // namespace guidatron
