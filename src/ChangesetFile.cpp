/*---------------------------------------------------------------------------------------------
 * guid-a-tron — the changeset container. See ChangesetFile.h for the layout and why.
 *--------------------------------------------------------------------------------------------*/
#include "ChangesetFile.h"

#include "Files.h"

#include <nlohmann/json.hpp>

#include <string.h>
#include <vector>

namespace guidatron {

namespace {

constexpr int PAGE = 64 * 1024;

//! `ChangesetLzmaHeader` (`BeSQLite/ChangesetFile.cpp:41`), which is private to that file. The
//! layout was checked against a real changeset: 16 00 | "ChangeSetLzma" 00 | 00 |
//! 00 pad | 10 00 | 02 00.
struct ChangesetLzmaHeader {
    uint16_t sizeOfHeader;
    char idString[15];
    uint16_t formatVersion;
    uint16_t compressionType;
};
static_assert(sizeof(ChangesetLzmaHeader) == 22, "the changeset file header is 22 bytes");

constexpr char const* LZMA_MARKER = "ChangeSetLzma";
constexpr uint16_t FORMAT_VERSION = 0x10;
constexpr uint16_t COMPRESSION_LZMA2 = 2;

void UIntToByteArrayBE(Byte bytes[4], uint32_t size) {
    bytes[0] = (Byte)((size >> 24) & 0xFF);
    bytes[1] = (Byte)((size >> 16) & 0xFF);
    bytes[2] = (Byte)((size >> 8) & 0xFF);
    bytes[3] = (Byte)(size & 0xFF);
}

uint32_t ByteArrayToUIntBE(Byte const bytes[4]) {
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) |
           (uint32_t)bytes[3];
}

} // namespace

//=============================================================================================
// ChangeStreamWriter
//=============================================================================================

//---------------------------------------------------------------------------------------------
DbResult ChangeStreamWriter::ReadFrom(ChangeStreamReader& reader) {
    std::vector<Byte> page((size_t)PAGE);
    for (;;) {
        int size = PAGE;
        DbResult rc = reader.Read(page.data(), &size);
        if (DB_OK != rc)
            return rc;
        if (size <= 0)
            return DB_OK;
        rc = Append(page.data(), size);
        if (DB_OK != rc)
            return rc;
    }
}

//=============================================================================================
// ChangesetFileReader
//=============================================================================================

//---------------------------------------------------------------------------------------------
DbResult ChangesetFileReader::Open(std::string const& path, std::string& error) {
    Close();
    if (!FileExists(path)) {
        error = "no such changeset: " + path;
        return DB_NOTFOUND;
    }
    if (!m_file.Open(path)) {
        error = "cannot read " + path;
        return DB_ERROR;
    }

    ChangesetLzmaHeader header;
    memset(&header, 0, sizeof(header));
    if (sizeof(header) != m_file.Read(&header, sizeof(header))) {
        error = path + " is too short to be a changeset";
        return DB_ERROR;
    }
    header.idString[sizeof(header.idString) - 1] = 0; // so a corrupt marker cannot run off the end
    if (header.sizeOfHeader != sizeof(header) || 0 != strcmp(header.idString, LZMA_MARKER) ||
        header.formatVersion != FORMAT_VERSION || header.compressionType != COMPRESSION_LZMA2) {
        error = path + " is not a changeset this tool knows how to read (marker, format version "
                       "0x10 and LZMA2 compression are all required)";
        return DB_ERROR;
    }

    if (!m_decoder.StartDecompress(m_file)) {
        error = "the LZMA2 stream in " + path + " could not be started";
        return DB_ERROR;
    }
    m_open = true;

    // The prefix: a big-endian length that counts the nul, then that many bytes.
    Byte sizeBytes[4];
    int at = 0;
    while (at < 4) {
        int size = 4 - at;
        if (!m_decoder.DecompressNextPage(sizeBytes + at, &size) || size <= 0) {
            error = "the prefix length could not be read from " + path;
            return DB_ERROR;
        }
        at += size;
    }
    uint32_t prefixSize = ByteArrayToUIntBE(sizeBytes);
    if (prefixSize == 0)
        return DB_OK;
    if (prefixSize > 64 * 1024 * 1024) {
        error = "the prefix in " + path + " claims an implausible length";
        return DB_ERROR;
    }

    std::vector<Byte> prefix((size_t)prefixSize, 0);
    uint32_t read = 0;
    while (read < prefixSize) {
        int size = (int)(prefixSize - read);
        if (!m_decoder.DecompressNextPage(prefix.data() + read, &size) || size <= 0) {
            error = "the prefix could not be read from " + path;
            return DB_ERROR;
        }
        read += (uint32_t)size;
    }
    // As the reader it replaces does: the bytes are taken as a C string, so the nul the length
    // counted is where the prefix ends.
    m_prefix.assign((char const*)prefix.data(),
                    strnlen((char const*)prefix.data(), (size_t)prefixSize));
    return DB_OK;
}

//---------------------------------------------------------------------------------------------
DbResult ChangesetFileReader::Read(Byte* data, int* size) {
    if (!m_open)
        return DB_ERROR;
    return m_decoder.DecompressNextPage(data, size) ? DB_OK : DB_ERROR;
}

//---------------------------------------------------------------------------------------------
void ChangesetFileReader::Close() {
    if (m_open)
        m_decoder.FinishDecompress();
    m_open = false;
    m_file.Close();
    m_prefix.clear();
}

//---------------------------------------------------------------------------------------------
// `GetSchemaChanges` reads the same two properties out of the prefix (`ChangesetFile.cpp:265`);
// only `ContainsSchemaChanges` is wanted here, because the DDL travels in the prefix's own bytes
// and this tool never applies it.
//---------------------------------------------------------------------------------------------
bool ChangesetFileReader::ContainsSchemaChanges() const {
    if (m_prefix.empty())
        return false;
    nlohmann::json parsed = nlohmann::json::parse(m_prefix, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object())
        return false;
    auto found = parsed.find("ContainsSchemaChanges");
    return found != parsed.end() && found->is_boolean() && found->get<bool>();
}

//=============================================================================================
// RawChangesetWriter
//=============================================================================================

//---------------------------------------------------------------------------------------------
DbResult RawChangesetWriter::Initialize() {
    if (m_started)
        return DB_ERROR;
    if (!m_file.Create(m_path))
        return DB_ERROR;
    m_started = true;

    ChangesetLzmaHeader header;
    memset(&header, 0, sizeof(header));
    header.sizeOfHeader = (uint16_t)sizeof(header);
    strcpy(header.idString, LZMA_MARKER);
    header.formatVersion = FORMAT_VERSION;
    header.compressionType = COMPRESSION_LZMA2;
    if (!m_file.Write(&header, sizeof(header)))
        return DB_ERROR;
    if (!m_encoder.StartCompress(m_file))
        return DB_ERROR;

    // The prefix, byte for byte as the original held it. The length counts the nul terminator,
    // which is what the reader expects: it reads `size` bytes and takes them as a C string.
    uint32_t size = m_prefix.empty() ? 0 : SizeWithNul(m_prefix);
    Byte sizeBytes[4];
    UIntToByteArrayBE(sizeBytes, size);
    if (!m_encoder.CompressNextPage(sizeBytes, 4))
        return DB_ERROR;
    if (size == 0)
        return DB_OK;
    return m_encoder.CompressNextPage(m_prefix.c_str(), (int)size) ? DB_OK : DB_ERROR;
}

//---------------------------------------------------------------------------------------------
DbResult RawChangesetWriter::Append(Byte const* data, int size) {
    if (!m_started)
        return DB_ERROR;
    return m_encoder.CompressNextPage(data, size) ? DB_OK : DB_ERROR;
}

//---------------------------------------------------------------------------------------------
DbResult RawChangesetWriter::Finish() {
    if (!m_started)
        return DB_OK;
    bool ok = m_encoder.FinishCompress();
    m_file.Close();
    m_started = false;
    return ok ? DB_OK : DB_ERROR;
}

} // namespace guidatron
