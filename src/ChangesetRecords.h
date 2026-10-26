/*---------------------------------------------------------------------------------------------
 * guid-a-tron — SQLite's changeset record format, field by field.
 *
 * SQLite's session extension can read the values in a changeset and apply a whole one, and iTwin's
 * wrapper around it can copy a changeset out byte for byte, but neither can write one *value*:
 * there is no API that replaces a field and leaves the rest of the stream alone. Rewriting
 * `bis_Element.FederationGuid` in place needs exactly that, so the format is implemented here —
 * from the specification in SQLite's own source, which this build compiles:
 *
 *   `sqlite3.c`'s RECORD FORMAT and CHANGESET FORMAT comments (search for "CHANGESET FORMAT";
 *   3.53.4 has them at line 233912), reproduced:
 *
 *     table header:  0x54 'T' | varint nCol | nCol bytes of PK flags | table name, nul-terminated
 *     change:        opcode (0x12 insert, 0x17 update, 0x09 delete) | indirect flag
 *                    | old record (delete, update) | new record (insert, update)
 *     record:        nCol fields, each  0x00 undefined | 0x01 int (8 bytes big-endian)
 *                    | 0x02 real (8 bytes big-endian) | 0x03 text (varint length + bytes, no nul)
 *                    | 0x04 blob (varint length + bytes) | 0x05 NULL
 *
 * Two properties of the format are what make the rewrite sound, and both are the format's own
 * words rather than an observation about this corpus:
 *
 *   - *"In the changeset format, the records that are part of INSERT or DELETE changes never
 *     contain any undefined fields"* — so a delete carries the deleted row's `FederationGuid`,
 *     which is the value `ChangesetScanner` reads and the one an `advance` cannot revise.
 *   - In an update's old record, *"all fields associated with table columns that are not PRIMARY
 *     KEY columns and are not modified by the UPDATE change are set to 'undefined'"* — so an
 *     update that does not touch the guid says nothing about it, and nothing has to be written.
 *
 * Varints are SQLite's (`sqlite3PutVarint`): 7 bits per byte, most significant group first, the
 * high bit meaning "another byte follows", and a ninth byte carrying a full 8 bits.
 *
 * Everything here is a view over caller-owned bytes: a `Field` points into the buffer it was
 * parsed from and is valid only while that buffer lives.
 *--------------------------------------------------------------------------------------------*/
#pragma once

#include "Types.h"

#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>

namespace guidatron {
namespace records {

//! The byte that leads a group of changes. Anything else at that position is a change opcode.
enum : Byte {
    TableLead = 0x54,    //!< 'T' — a changeset's table header
    PatchsetLead = 0x50, //!< 'P' — a patchset's; this tool does not accept patchsets
};
enum : Byte { OpInsert = 0x12, OpUpdate = 0x17, OpDelete = 0x09 };
enum : Byte {
    TypeUndefined = 0x00,
    TypeInt = 0x01,
    TypeReal = 0x02,
    TypeText = 0x03,
    TypeBlob = 0x04,
    TypeNull = 0x05,
};

//! The result of trying to parse one unit out of a buffer that may be short.
enum class Parse {
    Ok,        //!< parsed; `size` says how many bytes it took
    NeedMore,  //!< the buffer ends inside the unit; read more and try again
    Malformed, //!< not a unit of this format at all
};

//---------------------------------------------------------------------------------------------
//! Read a varint. Returns the bytes consumed, or 0 if `avail` ends inside it.
//---------------------------------------------------------------------------------------------
inline int GetVarint(Byte const* p, size_t avail, uint64_t& value) {
    uint64_t v = 0;
    for (size_t i = 0; i < 8; ++i) {
        if (i >= avail)
            return 0;
        Byte b = p[i];
        if ((b & 0x80) == 0) {
            value = (v << 7) | b;
            return (int)i + 1;
        }
        v = (v << 7) | (uint64_t)(b & 0x7f);
    }
    if (avail < 9)
        return 0;
    value = (v << 8) | p[8]; // the ninth byte carries all 8 bits
    return 9;
}

//---------------------------------------------------------------------------------------------
//! Write a varint, the way `sqlite3PutVarint` writes one. Returns the bytes written (at most 9).
//---------------------------------------------------------------------------------------------
inline int PutVarint(Byte* out, uint64_t value) {
    Byte buf[10];
    int n = 0;
    do {
        buf[n++] = (Byte)((value & 0x7f) | 0x80);
        value >>= 7;
    } while (value != 0);
    buf[0] &= 0x7f; // the last group written is the least significant, and it terminates the varint
    for (int i = 0; i < n; ++i)
        out[i] = buf[n - 1 - i];
    return n;
}

//! An 8-byte big-endian integer, which is how the format stores every integer and every real.
inline uint64_t GetU64BE(Byte const* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v = (v << 8) | p[i];
    return v;
}

//=============================================================================================
//! One field of one record: a view over the bytes it was parsed from, type byte included.
//=============================================================================================
struct Field {
    Byte const* start = nullptr; //!< the first byte of the field, which is its type byte
    int size = 0;                //!< the whole field's length, so `start + size` is the next field
    Byte type = TypeUndefined;
    Byte const* payload = nullptr; //!< the value's bytes: 8 for int/real, `payloadSize` for text/blob
    int payloadSize = 0;

    bool IsNull() const { return type == TypeNull; }
    //! Absent from the record, which in an update's old or new record means "not modified".
    bool IsUndefined() const { return type == TypeUndefined; }
    bool IsBlobOf(int bytes) const { return type == TypeBlob && payloadSize == bytes; }
    uint64_t AsUInt64() const { return type == TypeInt ? GetU64BE(payload) : 0; }
};

//---------------------------------------------------------------------------------------------
inline Parse ParseField(Byte const* p, size_t avail, Field& field) {
    if (avail < 1)
        return Parse::NeedMore;
    field.start = p;
    field.type = p[0];
    field.payload = nullptr;
    field.payloadSize = 0;
    switch (p[0]) {
    case TypeUndefined:
    case TypeNull:
        field.size = 1;
        return Parse::Ok;
    case TypeInt:
    case TypeReal:
        if (avail < 9)
            return Parse::NeedMore;
        field.payload = p + 1;
        field.payloadSize = 8;
        field.size = 9;
        return Parse::Ok;
    case TypeText:
    case TypeBlob: {
        uint64_t length = 0;
        int used = GetVarint(p + 1, avail - 1, length);
        if (used == 0)
            return Parse::NeedMore;
        if (length > (uint64_t)0x7fffffff)
            return Parse::Malformed;
        if (avail < (size_t)1 + (size_t)used + (size_t)length)
            return Parse::NeedMore;
        field.payload = p + 1 + used;
        field.payloadSize = (int)length;
        field.size = 1 + used + (int)length;
        return Parse::Ok;
    }
    default:
        return Parse::Malformed;
    }
}

//=============================================================================================
//! A table header: the only place a changeset names a table or says how wide its rows are.
//=============================================================================================
struct TableHeader {
    int nCol = 0;
    std::string name;
    Byte const* pkFlags = nullptr; //!< `nCol` bytes, 1 where the column is part of the primary key
    int size = 0;

    //! Is `column` the table's one and only primary key column?
    bool IsOnlyPkColumn(int column) const {
        if (pkFlags == nullptr || column < 0 || column >= nCol)
            return false;
        for (int i = 0; i < nCol; ++i)
            if ((pkFlags[i] != 0) != (i == column))
                return false;
        return true;
    }
};

//---------------------------------------------------------------------------------------------
inline Parse ParseTableHeader(Byte const* p, size_t avail, TableHeader& header) {
    if (avail < 1)
        return Parse::NeedMore;
    if (p[0] != TableLead)
        return Parse::Malformed;
    uint64_t nCol = 0;
    int used = GetVarint(p + 1, avail - 1, nCol);
    if (used == 0)
        return Parse::NeedMore;
    if (nCol == 0 || nCol > 65535)
        return Parse::Malformed;
    size_t at = (size_t)1 + (size_t)used;
    if (avail < at + nCol)
        return Parse::NeedMore;
    Byte const* pk = p + at;
    at += (size_t)nCol;
    size_t nameAt = at;
    while (at < avail && p[at] != 0)
        ++at;
    if (at >= avail)
        return Parse::NeedMore; // the name's nul terminator has not arrived yet
    header.nCol = (int)nCol;
    header.pkFlags = pk;
    header.name.assign((char const*)p + nameAt, at - nameAt);
    header.size = (int)(at + 1);
    return Parse::Ok;
}

//! Append a field holding `bytes` as a blob value — the one kind of field this tool writes.
inline void AppendBlobField(std::vector<Byte>& out, void const* bytes, int size) {
    out.push_back(TypeBlob);
    Byte varint[10];
    int used = PutVarint(varint, (uint64_t)size);
    out.insert(out.end(), varint, varint + used);
    Byte const* from = (Byte const*)bytes;
    out.insert(out.end(), from, from + size);
}

//! How many records a change carries: an update has old and new, an insert or delete has one.
inline int RecordsPerChange(Byte opcode) { return opcode == OpUpdate ? 2 : 1; }

inline bool IsOpcode(Byte b) { return b == OpInsert || b == OpUpdate || b == OpDelete; }

inline char const* OpcodeName(Byte b) {
    switch (b) {
    case OpInsert: return "insert";
    case OpUpdate: return "update";
    case OpDelete: return "delete";
    }
    return "?";
}

} // namespace records
} // namespace guidatron
