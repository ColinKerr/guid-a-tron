/*---------------------------------------------------------------------------------------------
 * guid-a-tron — the changeset file: a container around a change stream.
 *
 * The layout, from `BeSQLite/ChangesetFile.cpp:41-110`
 *
 *     22-byte header   16 00 | "ChangeSetLzma" 00 | 00 00 | 10 00 | 02 00
 *                      (size of header, marker, format version 0x10, compression type 2 = LZMA2)
 *     LZMA2 stream     4-byte big-endian prefix length
 *                      prefix bytes, **including the nul terminator** the length counts
 *                      the change stream, to the end
 *
 * The prefix is a small JSON document — `{"ContainsSchemaChanges":true,"DDL":"..."}` — and it is
 * empty for the great majority of changesets.
 *
 * **Why this tool has its own writer.** `ChangesetFileWriter` rebuilds the prefix rather than
 * carrying it: it constructs a fresh `BeJsDocument` and forces `ContainsSchemaChanges` true
 * whenever there is DDL, so a schema changeset round-tripped through it comes out with different
 * prefix bytes and therefore a different id. A changeset's id is a SHA-1 over the prefix and every
 * decompressed byte, so carrying the original bytes is what lets a no-op rewrite reproduce every
 * id — which is this tool's own test that its format handling is byte-exact.
 *--------------------------------------------------------------------------------------------*/
#pragma once

#include "Lzma.h"
#include "Sqlite.h"
#include "Types.h"

#include <string>

namespace guidatron {

//=============================================================================================
//! A source of change-stream bytes: the decompressed contents of a changeset, or a rewrite of it.
//=============================================================================================
struct ChangeStreamReader {
    virtual ~ChangeStreamReader() {}
    //! Up to `*size` bytes. `*size` comes back as how many were read, and **0 means the end**.
    virtual DbResult Read(Byte* data, int* size) = 0;
};

//=============================================================================================
//! A sink for change-stream bytes: a file to write, or a hash to feed.
//=============================================================================================
struct ChangeStreamWriter {
    virtual ~ChangeStreamWriter() {}
    virtual DbResult Append(Byte const* data, int size) = 0;
    //! Pump `reader` to the end, in page-sized pieces.
    DbResult ReadFrom(ChangeStreamReader& reader);
};

//=============================================================================================
//! Reads a changeset file. Opening it reads the header and the prefix; `Read` yields the rest.
//=============================================================================================
struct ChangesetFileReader : ChangeStreamReader {
    //! Validates the header and reads the prefix. DB_NOTFOUND, or DB_ERROR with `error` set.
    DbResult Open(std::string const& path, std::string& error);
    //! The prefix's bytes, verbatim, without the nul. Empty for a changeset that has none.
    std::string const& Prefix() const { return m_prefix; }
    DbResult Read(Byte* data, int* size) override;
    void Close();

    //! `ContainsSchemaChanges` out of the prefix JSON, which is what the manifest records.
    bool ContainsSchemaChanges() const;

private:
    FileLzmaInput m_file;
    LzmaDecoder m_decoder;
    std::string m_prefix;
    bool m_open = false;
};

//=============================================================================================
/**
 * Writes a changeset file, carrying the prefix's bytes instead of rebuilding them.
 *
 * "Raw" is the prefix: whatever bytes the original held are what get written, nul included. See the
 * header comment for why that matters.
 */
//=============================================================================================
struct RawChangesetWriter : ChangeStreamWriter {
    RawChangesetWriter(std::string path, std::string prefix)
        : m_path(std::move(path)), m_prefix(std::move(prefix)) {}
    ~RawChangesetWriter() override { Finish(); }

    //! Writes the header and the prefix. Overwrites `path` if it is there.
    DbResult Initialize();
    DbResult Append(Byte const* data, int size) override;
    //! Writes the end-of-stream marker and closes. Safe to call twice.
    DbResult Finish();

private:
    std::string m_path;
    std::string m_prefix;
    FileLzmaOutput m_file;
    LzmaEncoder m_encoder;
    bool m_started = false;
};

} // namespace guidatron
