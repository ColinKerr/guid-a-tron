/*---------------------------------------------------------------------------------------------
 * guid-a-tron — the changeset id, computed the way iTwin computes it.
 *
 * `ChangesetProps::ComputeChangesetId` lives in iModelPlatform
 * (`DgnCore/ChangesetTxns.cpp:724-772`), which this tool does not link — it links nothing of
 * iTwin's. The algorithm is reproduced here rather than approximated, because a changeset whose id
 * is computed differently is a changeset iTwin.js will reject: its apply recomputes the id from the
 * parent and the bytes and refuses a mismatch (`ChangesetProps::ValidateContent`).
 *
 *     SHA1( parentId as 20 raw bytes, or 20 zero bytes when there is no parent )
 *         + ( the prefix string **including its nul terminator**, when it is not empty )
 *         + ( every byte of the decompressed change stream )
 *
 * Reading the stream through a `ChangesetFileReader` is what makes the third term the same bytes
 * iTwin hashes: the reader decompresses, and `Append` sees exactly what the writer wrote.
 *
 *--------------------------------------------------------------------------------------------*/
#pragma once

#include "ChangesetFile.h"
#include "Sha1.h"
#include "Types.h"

#include <string.h>
#include <string>

namespace guidatron {

struct ChangesetIdGenerator : ChangeStreamWriter {
    Sha1 m_hash;

    //! `parentId` is a 40-character hex SHA-1, or empty for the first changeset of a timeline.
    void AddParentId(std::string const& parentId) {
        Byte bytes[Sha1::HashBytes];
        if (parentId.empty() || parentId.length() != Sha1::HashBytes * 2) {
            memset(bytes, 0, sizeof(bytes));
        } else {
            auto hexToInt = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return 0;
            };
            for (int i = 0; i < (int)Sha1::HashBytes; ++i)
                bytes[i] = (Byte)(16 * hexToInt(parentId[2 * i]) + hexToInt(parentId[2 * i + 1]));
        }
        m_hash.Add(bytes, Sha1::HashBytes);
    }

    //! The prefix counts for the id, nul included — see the header comment.
    void AddPrefix(std::string const& prefix) {
        if (!prefix.empty())
            Append((Byte const*)prefix.c_str(), (int)SizeWithNul(prefix));
    }

    DbResult Append(Byte const* data, int size) override {
        m_hash.Add(data, (size_t)size);
        return DB_OK;
    }

    std::string Id() const { return m_hash.GetHashString(); }
};

//! The id of a changeset file, given the id of the changeset it follows. Empty string on failure.
inline std::string ComputeChangesetId(std::string const& parentId, std::string const& changesetFile) {
    ChangesetFileReader reader;
    std::string error;
    if (DB_OK != reader.Open(changesetFile, error))
        return "";

    ChangesetIdGenerator generator;
    generator.AddParentId(parentId);
    generator.AddPrefix(reader.Prefix());
    if (DB_OK != generator.ReadFrom(reader))
        return "";
    return generator.Id();
}

} // namespace guidatron
