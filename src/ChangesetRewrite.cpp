/*---------------------------------------------------------------------------------------------
 * guid-a-tron — the rewriter. See ChangesetRewrite.h for why, and ChangesetRecords.h
 * for the format this file reads and writes.
 *--------------------------------------------------------------------------------------------*/
#include "ChangesetRewrite.h"
#include "ChangesetFile.h"
#include "ChangesetId.h"
#include "ChangesetRecords.h"
#include "Files.h"

#include <algorithm>
#include <string.h>
#include <stdio.h>

namespace guidatron {

namespace {
//! How much is pulled from the decompressor at a time. The parser holds at most this plus the
//! largest single change, so peak memory is a function of the widest row, not of the changeset.
constexpr int READ_CHUNK = 64 * 1024;
//! A guid field is a 16-byte blob, and nothing in the format says so: the width is the check.
constexpr int GUID_BYTES = 16;
} // namespace

//---------------------------------------------------------------------------------------------
void RewriteCounts::Add(RewriteCounts const& o) {
    tables += o.tables;
    changes += o.changes;
    elementInserts += o.elementInserts;
    elementUpdates += o.elementUpdates;
    elementDeletes += o.elementDeletes;
    insertsWithoutGuid += o.insertsWithoutGuid;
    deletesWithoutGuid += o.deletesWithoutGuid;
    updateOldsWithoutGuid += o.updateOldsWithoutGuid;
    updateNewsWithoutGuid += o.updateNewsWithoutGuid;
    fieldsWritten += o.fieldsWritten;
    unresolved += o.unresolved;
    guidsAssigned += o.guidsAssigned;
    newIncarnations += o.newIncarnations;
    insertsWhileAlive += o.insertsWhileAlive;
    guidsObserved += o.guidsObserved;
    unexpectedGuidValues += o.unexpectedGuidValues;
    bytesIn += o.bytesIn;
    bytesOut += o.bytesOut;
}

//=============================================================================================
// ElementGuidStore
//=============================================================================================

//---------------------------------------------------------------------------------------------
DbResult ElementGuidStore::CreateTables() {
    // `guid_assignment` holds the current incarnation of every id this tool has given a guid to;
    // `guid_retired` holds the incarnations a reused id left behind, so the history of a guid is
    // readable afterwards rather than only inferable.
    char const* ddl =
        "CREATE TABLE guid_assignment("
        " ElementId INTEGER PRIMARY KEY,"
        " Guid BLOB NOT NULL,"
        " Incarnation INTEGER NOT NULL,"
        " Alive INTEGER NOT NULL,"
        " Assigned INTEGER NOT NULL,"  // 0: the original's own guid, read out of a changeset
        " SinceIndex INTEGER NOT NULL);"
        "CREATE TABLE guid_retired("
        " ElementId INTEGER NOT NULL,"
        " Incarnation INTEGER NOT NULL,"
        " Guid BLOB NOT NULL,"
        " SinceIndex INTEGER NOT NULL,"
        " RetiredIndex INTEGER NOT NULL,"
        " PRIMARY KEY(ElementId, Incarnation));"
        "CREATE TABLE store_meta(Name TEXT PRIMARY KEY, Value TEXT NOT NULL);";
    return m_db.ExecuteSql(ddl);
}

//---------------------------------------------------------------------------------------------
DbResult ElementGuidStore::LoadEntries() {
    Statement stmt;
    DbResult rc = stmt.Prepare(
        m_db, "SELECT ElementId, Guid, Incarnation, Alive, Assigned, SinceIndex FROM guid_assignment");
    if (DB_OK != rc)
        return rc;
    while (DB_ROW == stmt.Step()) {
        GuidRecord record;
        uint64_t id = stmt.GetValueUInt64(0);
        record.guid = stmt.GetValueGuid(1);
        record.incarnation = (uint32_t)stmt.GetValueInt64(2);
        record.alive = 0 != stmt.GetValueInt(3);
        record.assigned = 0 != stmt.GetValueInt(4);
        record.sinceIndex = stmt.GetValueInt64(5);
        m_entries[id] = record;
        if (record.assigned)
            ++m_assigned;
    }
    Statement retired;
    if (DB_OK == retired.Prepare(m_db, "SELECT COUNT(*) FROM guid_retired"))
        if (DB_ROW == retired.Step())
            m_retired = retired.GetValueInt64(0);
    return DB_OK;
}

//---------------------------------------------------------------------------------------------
DbResult ElementGuidStore::Open(std::string const& path, GuidPolicy policy, GuidCR scope,
                                bool createIfMissing, std::string& error, bool policyIsExplicit) {
    m_policy = policy;
    m_scope = scope;
    bool exists = FileExists(path);
    if (!exists && !createIfMissing) {
        error = "no guid store at " + path + " — run `guid-a-tron seed` first";
        return DB_NOTFOUND;
    }

    if (!exists) {
        DbResult rc = m_db.CreateNewDb(path);
        if (DB_OK != rc) {
            error = "cannot create the guid store";
            return rc;
        }
        rc = CreateTables();
        if (DB_OK != rc) {
            error = "cannot initialize the guid store";
            return rc;
        }
        SetMeta("guidPolicy", policy == GuidPolicy::Derived ? "derived" : "random");
        SetMeta("derivationScope", scope.ToString());
        SetMeta("tool", "guid-a-tron rewrite");
        return m_db.SaveChanges("guid-a-tron: guid store");
    }

    DbResult rc = m_db.OpenDb(path, Db::OpenMode::ReadWrite);
    if (DB_OK != rc) {
        error = "cannot open the guid store read-write";
        return rc;
    }
    std::string storedPolicy, storedScope;
    GetMeta("guidPolicy", storedPolicy);
    GetMeta("derivationScope", storedScope);
    std::string asked(policy == GuidPolicy::Derived ? "derived" : "random");
    if (policyIsExplicit && !storedPolicy.empty() && storedPolicy != asked) {
        error = "the store was created with --guids " + std::string(storedPolicy.c_str()) +
                ", not " + std::string(asked.c_str());
        return DB_MISMATCH;
    }
    if (!storedPolicy.empty())
        m_policy = storedPolicy == "derived" ? GuidPolicy::Derived : GuidPolicy::Random;
    if (!storedScope.empty())
        m_scope.FromString(storedScope.c_str());
    return LoadEntries();
}

//---------------------------------------------------------------------------------------------
void ElementGuidStore::Close() {
    if (m_db.IsDbOpen()) {
        Flush();
        m_db.CloseDb();
    }
    m_entries.clear();
    m_dirty.clear();
    m_pendingRetired.clear();
}

//---------------------------------------------------------------------------------------------
DbResult ElementGuidStore::Flush() {
    if (!m_db.IsDbOpen())
        return DB_ERROR;
    if (m_dirty.empty() && m_pendingRetired.empty())
        return m_db.SaveChanges();

    Statement upsert;
    DbResult rc = upsert.Prepare(m_db, "INSERT OR REPLACE INTO guid_assignment"
                                       "(ElementId,Guid,Incarnation,Alive,Assigned,SinceIndex)"
                                       " VALUES(?,?,?,?,?,?)");
    if (DB_OK != rc)
        return rc;
    for (uint64_t id : m_dirty) {
        auto found = m_entries.find(id);
        if (found == m_entries.end())
            continue;
        GuidRecord const& record = found->second;
        upsert.Reset();
        upsert.ClearBindings();
        upsert.BindUInt64(1, id);
        upsert.BindGuid(2, record.guid);
        upsert.BindInt64(3, (int64_t)record.incarnation);
        upsert.BindInt(4, record.alive ? 1 : 0);
        upsert.BindInt(5, record.assigned ? 1 : 0);
        upsert.BindInt64(6, record.sinceIndex);
        rc = upsert.Step();
        if (DB_DONE != rc)
            return rc;
    }
    m_dirty.clear();

    if (!m_pendingRetired.empty()) {
        Statement retire;
        rc = retire.Prepare(m_db, "INSERT OR REPLACE INTO guid_retired"
                                  "(ElementId,Incarnation,Guid,SinceIndex,RetiredIndex)"
                                  " VALUES(?,?,?,?,?)");
        if (DB_OK != rc)
            return rc;
        for (RetiredRow const& row : m_pendingRetired) {
            retire.Reset();
            retire.ClearBindings();
            retire.BindUInt64(1, row.elementId);
            retire.BindInt64(2, (int64_t)row.incarnation);
            retire.BindGuid(3, row.guid);
            retire.BindInt64(4, row.sinceIndex);
            retire.BindInt64(5, row.retiredIndex);
            rc = retire.Step();
            if (DB_DONE != rc)
                return rc;
        }
        m_pendingRetired.clear();
    }
    return m_db.SaveChanges("guid-a-tron: guid store");
}

//---------------------------------------------------------------------------------------------
bool ElementGuidStore::Lookup(uint64_t elementId, Guid& guid) const {
    auto found = m_entries.find(elementId);
    if (found == m_entries.end())
        return false;
    guid = found->second.guid;
    return guid.IsValid();
}

//---------------------------------------------------------------------------------------------
Guid ElementGuidStore::ChooseGuid(uint64_t elementId, uint32_t incarnation) const {
    // Random is the default and the production-faithful one: a connector generates a guid when it
    // creates an element, as the platform itself does on insert. Derived exists so a
    // fork can be rebuilt instead of archived, and takes the incarnation so that a reused id still
    // gets two different guids.
    return m_policy == GuidPolicy::Derived ? DeriveElementGuid(m_scope, elementId, incarnation)
                                           : Guid(true);
}

//---------------------------------------------------------------------------------------------
Guid ElementGuidStore::NewIncarnation(uint64_t elementId, int64_t index, bool& previousWasAlive,
                                        uint32_t& incarnation) {
    previousWasAlive = false;
    incarnation = 1;
    auto found = m_entries.find(elementId);
    if (found != m_entries.end()) {
        previousWasAlive = found->second.alive;
        incarnation = found->second.incarnation + 1;
        // The dead incarnation's guid is kept: it is the right answer for any change still to be
        // read that refers to the element that used to own this id.
        m_pendingRetired.push_back(
            {elementId, found->second.incarnation, found->second.guid, found->second.sinceIndex, index});
        ++m_retired;
    }
    GuidRecord record;
    record.guid = ChooseGuid(elementId, incarnation);
    record.incarnation = incarnation;
    record.alive = true;
    record.assigned = true;
    record.sinceIndex = index;
    m_entries[elementId] = record;
    m_dirty.insert(elementId);
    ++m_assigned;
    return record.guid;
}

//---------------------------------------------------------------------------------------------
void ElementGuidStore::Observe(uint64_t elementId, GuidCR guid, int64_t index) {
    // Only ids the store already knows: an element whose insert carried a guid carries it in its
    // delete's old values too, so it never needs an entry. Updating the ones that do exist keeps
    // `alive` honest, which is what makes reuse detectable.
    auto found = m_entries.find(elementId);
    if (found == m_entries.end())
        return;
    if (found->second.guid == guid && found->second.alive)
        return;
    if (!found->second.alive) {
        // A dead id coming back with the original's own guid is still a new incarnation, and the
        // guid it replaces is still retired — otherwise the audit trail would have a hole in
        // exactly the case that is hardest to reason about.
        m_pendingRetired.push_back({elementId, found->second.incarnation, found->second.guid,
                                    found->second.sinceIndex, index});
        ++m_retired;
        ++found->second.incarnation;
    }
    found->second.guid = guid;
    found->second.alive = true;
    found->second.assigned = false;
    found->second.sinceIndex = index;
    m_dirty.insert(elementId);
}

//---------------------------------------------------------------------------------------------
void ElementGuidStore::MarkDeleted(uint64_t elementId, int64_t index) {
    auto found = m_entries.find(elementId);
    if (found == m_entries.end() || !found->second.alive)
        return;
    found->second.alive = false;
    found->second.sinceIndex = index;
    m_dirty.insert(elementId);
}

//---------------------------------------------------------------------------------------------
bool ElementGuidStore::GetMeta(char const* name, std::string& value) const {
    value.clear();
    Statement stmt;
    if (DB_OK != stmt.Prepare(m_db, "SELECT Value FROM store_meta WHERE Name=?"))
        return false;
    stmt.BindText(1, name);
    if (DB_ROW != stmt.Step())
        return false;
    value = stmt.GetValueText(0);
    return true;
}

DbResult ElementGuidStore::SetMeta(char const* name, std::string const& value) {
    Statement stmt;
    DbResult rc = stmt.Prepare(m_db, "INSERT OR REPLACE INTO store_meta(Name,Value) VALUES(?,?)");
    if (DB_OK != rc)
        return rc;
    stmt.BindText(1, name);
    stmt.BindText(2, value.c_str());
    rc = stmt.Step();
    return rc == DB_DONE ? DB_OK : rc;
}

int64_t ElementGuidStore::LastIndex() const {
    std::string value;
    if (!GetMeta("lastIndex", value) || value.empty())
        return 0;
    return atoll(value.c_str());
}

std::string ElementGuidStore::LastChangesetId() const {
    std::string value;
    GetMeta("lastChangesetId", value);
    return value;
}

DbResult ElementGuidStore::RecordProgress(int64_t index, std::string const& changesetId) {
    std::string text = Format("%lld", (long long)index);
    DbResult rc = SetMeta("lastIndex", text);
    if (DB_OK != rc)
        return rc;
    return SetMeta("lastChangesetId", changesetId);
}

//=============================================================================================
// The rewriter itself: a reader that reads one change stream and produces another.
//=============================================================================================
namespace {

struct RewritingReader : ChangeStreamReader {
    RewritingReader(ChangeStreamReader& source, ElementGuidStore& store,
                    RewriteOptions const& options, RewriteCounts& counts)
        : m_source(source), m_store(store), m_options(options), m_counts(counts) {}

    DbResult Read(Byte* data, int* pSize) override {
        int capacity = *pSize;
        int written = 0;
        while (written < capacity) {
            if (m_outPos < m_out.size()) {
                size_t take = std::min((size_t)(capacity - written), m_out.size() - m_outPos);
                memcpy(data + written, m_out.data() + m_outPos, take);
                written += (int)take;
                m_outPos += take;
                continue;
            }
            m_out.clear();
            m_outPos = 0;
            if (!Advance())
                break;
        }
        if (DB_OK != m_status)
            return m_status;
        m_counts.bytesOut += written;
        *pSize = written;
        return DB_OK;
    }

    DbResult Status() const { return m_status; }
    std::string const& Error() const { return m_error; }

private:
    //! Produce the next unit, or report that the stream is over.
    bool Advance() {
        for (;;) {
            if (DB_OK != m_status)
                return false;
            if (m_in.size() > m_inPos) {
                records::Parse result = NextUnit();
                if (result == records::Parse::Ok)
                    return true;
                if (result == records::Parse::Malformed)
                    return false;
                // NeedMore: fall through and read.
            }
            if (m_sourceDone) {
                if (m_in.size() != m_inPos)
                    Fail("the change stream ends in the middle of a record");
                return false;
            }
            if (!Fill())
                return false;
        }
    }

    bool Fill() {
        if (m_inPos > 0) {
            m_in.erase(m_in.begin(), m_in.begin() + (long)m_inPos);
            m_inPos = 0;
        }
        size_t had = m_in.size();
        m_in.resize(had + READ_CHUNK);
        int size = READ_CHUNK;
        DbResult rc = m_source.Read(m_in.data() + had, &size);
        if (DB_OK != rc || size < 0) {
            m_in.resize(had);
            m_status = DB_OK != rc ? rc : DB_ERROR;
            m_error = "reading the original's change stream failed";
            return false;
        }
        m_in.resize(had + (size_t)size);
        m_counts.bytesIn += size;
        if (size == 0)
            m_sourceDone = true;
        return true;
    }

    void Fail(char const* what) {
        m_status = DB_ERROR;
        if (m_error.empty())
            m_error = what;
    }

    records::Parse NextUnit() {
        Byte const* p = m_in.data() + m_inPos;
        size_t avail = m_in.size() - m_inPos;
        if (p[0] == records::TableLead) {
            records::TableHeader header;
            records::Parse result = records::ParseTableHeader(p, avail, header);
            if (result != records::Parse::Ok) {
                if (result == records::Parse::Malformed)
                    Fail("a table header this tool cannot read");
                return result;
            }
            m_nCol = header.nCol;
            m_target = header.name == m_options.table;
            if (m_target && m_nCol <= m_options.guidColumn) {
                Fail("the target table has fewer columns than the guid column's ordinal");
                return records::Parse::Malformed;
            }
            Emit(p, header.size);
            m_inPos += (size_t)header.size;
            ++m_counts.tables;
            return records::Parse::Ok;
        }
        if (p[0] == records::PatchsetLead) {
            Fail("this is a patchset, not a changeset: it does not carry the old values a rewrite "
                 "needs");
            return records::Parse::Malformed;
        }
        if (!records::IsOpcode(p[0])) {
            Fail("neither a table header nor a change opcode");
            return records::Parse::Malformed;
        }
        return NextChange(p, avail);
    }

    records::Parse NextChange(Byte const* p, size_t avail) {
        if (avail < 2)
            return records::Parse::NeedMore;
        if (m_nCol == 0) {
            Fail("a change before any table header");
            return records::Parse::Malformed;
        }
        m_opcode = p[0];
        int recordCount = records::RecordsPerChange(m_opcode);
        size_t at = 2;
        m_fields.assign((size_t)m_nCol * recordCount, records::Field());
        for (int r = 0; r < recordCount; ++r) {
            for (int c = 0; c < m_nCol; ++c) {
                records::Field& field = m_fields[(size_t)r * m_nCol + c];
                records::Parse result = records::ParseField(p + at, avail - at, field);
                if (result != records::Parse::Ok) {
                    if (result == records::Parse::Malformed)
                        Fail("a field type this format does not define");
                    return result;
                }
                at += (size_t)field.size;
            }
        }
        ++m_counts.changes;

        m_replaceOld = false;
        m_replaceNew = false;
        if (m_target && !Plan())
            return records::Parse::Malformed;

        if (m_replaceOld || m_replaceNew)
            EmitRewritten(p, recordCount);
        else
            Emit(p, (int)at);
        m_inPos += at;
        return records::Parse::Ok;
    }

    records::Field const& Old(int column) const { return m_fields[(size_t)column]; }
    records::Field const& New(int column) const {
        size_t base = m_opcode == records::OpUpdate ? (size_t)m_nCol : 0;
        return m_fields[base + (size_t)column];
    }
    static Guid GuidOf(records::Field const& field) {
        Guid guid;
        memcpy(guid.b, field.payload, sizeof(guid.b));
        return guid;
    }
    //! A guid field is NULL, or a 16-byte blob, or the column ordinal is wrong — and if it is
    //! wrong, every assumption in this file is too, so say so rather than writing something.
    bool CheckGuidField(records::Field const& field) {
        if (field.IsNull() || field.IsUndefined() || field.IsBlobOf(GUID_BYTES))
            return true;
        ++m_counts.unexpectedGuidValues;
        Fail("the guid column holds something that is not a guid — check --guid-column");
        return false;
    }

    /**
     * Decide what this change needs, and tell the store what it now knows.
     *
     * The rule, in one line: **every NULL FederationGuid in the target table, in an old record or a
     * new one, becomes the guid that element's current incarnation holds in the fork.** Everything
     * else travels untouched.
     */
    bool Plan() {
        int gc = m_options.guidColumn;
        records::Field const& id = Old(m_options.idColumn); // the PK is in the first record always
        if (id.type != records::TypeInt) {
            Fail("the id column is not an integer");
            return false;
        }
        uint64_t elementId = id.AsUInt64();

        switch (m_opcode) {
        case records::OpInsert: {
            ++m_counts.elementInserts;
            records::Field const& guid = New(gc);
            if (!CheckGuidField(guid))
                return false;
            if (guid.IsBlobOf(GUID_BYTES)) {
                // The original brought its own guid: nothing to write, but the store should know.
                if (m_options.substitute) {
                    ++m_counts.guidsObserved;
                    m_store.Observe(elementId, GuidOf(guid), m_options.index);
                }
                return true;
            }
            ++m_counts.insertsWithoutGuid;
            if (!m_options.substitute)
                return true; // scanning: the count is the whole point, the bytes stay as they are
            bool previousWasAlive = false;
            uint32_t incarnation = 1;
            m_newGuid = m_store.NewIncarnation(elementId, m_options.index, previousWasAlive,
                                               incarnation);
            ++m_counts.guidsAssigned;
            if (incarnation > 1)
                ++m_counts.newIncarnations;
            if (previousWasAlive)
                ++m_counts.insertsWhileAlive;
            m_replaceNew = true;
            ++m_counts.fieldsWritten;
            return true;
        }
        case records::OpDelete: {
            ++m_counts.elementDeletes;
            records::Field const& guid = Old(gc);
            if (!CheckGuidField(guid))
                return false;
            if (guid.IsNull()) {
                ++m_counts.deletesWithoutGuid;
                if (m_options.substitute) {
                    if (m_store.Lookup(elementId, m_oldGuid)) {
                        m_replaceOld = true;
                        ++m_counts.fieldsWritten;
                    } else {
                        ++m_counts.unresolved;
                    }
                }
            }
            if (m_options.substitute)
                m_store.MarkDeleted(elementId, m_options.index);
            return true;
        }
        case records::OpUpdate: {
            ++m_counts.elementUpdates;
            records::Field const& oldGuid = Old(gc);
            records::Field const& newGuid = New(gc);
            if (!CheckGuidField(oldGuid) || !CheckGuidField(newGuid))
                return false;
            // An update that does not touch the guid leaves both fields undefined, which is every
            if (oldGuid.IsNull()) {
                // The original is filling a guid in itself; the fork already has one there.
                ++m_counts.updateOldsWithoutGuid;
                if (m_options.substitute) {
                    if (m_store.Lookup(elementId, m_oldGuid)) {
                        m_replaceOld = true;
                        ++m_counts.fieldsWritten;
                    } else {
                        ++m_counts.unresolved;
                    }
                }
            }
            if (newGuid.IsBlobOf(GUID_BYTES)) {
                if (m_options.substitute) {
                    ++m_counts.guidsObserved;
                    m_store.Observe(elementId, GuidOf(newGuid), m_options.index);
                }
            } else if (newGuid.IsNull()) {
                // The original is clearing the guid. The fork keeps one, because an element without
                // a guid is the thing this whole exercise exists to remove.
                ++m_counts.updateNewsWithoutGuid;
                if (m_options.substitute) {
                    if (m_store.Lookup(elementId, m_newGuid)) {
                        // keep what the fork holds
                    } else if (oldGuid.IsBlobOf(GUID_BYTES)) {
                        m_newGuid = GuidOf(oldGuid); // keep what the original held
                    } else {
                        bool previousWasAlive = false;
                        uint32_t incarnation = 1;
                        m_newGuid = m_store.NewIncarnation(elementId, m_options.index,
                                                           previousWasAlive, incarnation);
                        ++m_counts.guidsAssigned;
                    }
                    m_replaceNew = true;
                    ++m_counts.fieldsWritten;
                }
            }
            return true;
        }
        default:
            Fail("an opcode this format does not define");
            return false;
        }
    }

    void Emit(Byte const* from, int size) { m_out.insert(m_out.end(), from, from + size); }

    //! The change again, field by field, with the guid fields this rewrite chose replaced.
    void EmitRewritten(Byte const* p, int recordCount) {
        m_out.push_back(m_opcode);
        m_out.push_back(p[1]); // the indirect-change flag is the original's
        for (int r = 0; r < recordCount; ++r) {
            bool isOldRecord = m_opcode == records::OpDelete ||
                               (m_opcode == records::OpUpdate && r == 0);
            for (int c = 0; c < m_nCol; ++c) {
                records::Field const& field = m_fields[(size_t)r * m_nCol + c];
                bool replace = c == m_options.guidColumn &&
                               (isOldRecord ? m_replaceOld : m_replaceNew);
                if (replace)
                    records::AppendBlobField(m_out, isOldRecord ? m_oldGuid.b : m_newGuid.b,
                                             (int)sizeof(m_oldGuid.b));
                else
                    Emit(field.start, field.size);
            }
        }
    }

    ChangeStreamReader& m_source;
    ElementGuidStore& m_store;
    RewriteOptions m_options;
    RewriteCounts& m_counts;

    std::vector<Byte> m_in;
    size_t m_inPos = 0;
    std::vector<Byte> m_out;
    size_t m_outPos = 0;
    bool m_sourceDone = false;
    DbResult m_status = DB_OK;
    std::string m_error;

    int m_nCol = 0;
    bool m_target = false;
    Byte m_opcode = 0;
    std::vector<records::Field> m_fields;
    bool m_replaceOld = false, m_replaceNew = false;
    Guid m_oldGuid, m_newGuid;
};

//! Stream an open changeset through the rewriter into `target`, which may be a file writer or an
//! id generator. One decompression pass: the prefix has already come out of `file`.
DbResult StreamThrough(ChangesetFileReader& file, ChangeStreamWriter& target, ElementGuidStore& store,
                       RewriteOptions const& options, RewriteCounts& counts, std::string& error) {
    RewritingReader rewriter(file, store, options, counts);
    DbResult rc = target.ReadFrom(rewriter);
    if (DB_OK != rewriter.Status()) {
        error = rewriter.Error();
        return rewriter.Status();
    }
    if (DB_OK != rc)
        error = "writing the rewritten change stream failed";
    return rc;
}

} // namespace

//---------------------------------------------------------------------------------------------
DbResult RewriteChangeset(std::string const& in, std::string const& out, ElementGuidStore& store,
                          RewriteOptions const& options, RewriteCounts& counts,
                          std::string& error) {
    ChangesetFileReader file;
    DbResult rc = file.Open(in, error);
    if (DB_OK != rc)
        return rc;

    // The prefix travels into the output untouched — see ChangesetFile.h.
    RawChangesetWriter writer(out, file.Prefix());
    rc = writer.Initialize();
    if (DB_OK != rc) {
        error = "cannot start writing " + out;
        return rc;
    }
    rc = StreamThrough(file, writer, store, options, counts, error);
    DbResult finished = writer.Finish();
    return DB_OK != rc ? rc : finished;
}

//---------------------------------------------------------------------------------------------
DbResult RewrittenChangesetId(std::string const& in, std::string const& parentId,
                              ElementGuidStore& store, RewriteOptions const& options,
                              RewriteCounts& counts, std::string& id, std::string& error) {
    ChangesetFileReader file;
    DbResult rc = file.Open(in, error);
    if (DB_OK != rc)
        return rc;

    ChangesetIdGenerator generator;
    generator.AddParentId(parentId);
    generator.AddPrefix(file.Prefix());
    rc = StreamThrough(file, generator, store, options, counts, error);
    if (DB_OK != rc)
        return rc;
    id = generator.Id();
    return DB_OK;
}

//---------------------------------------------------------------------------------------------
int FindColumnOrdinal(Db& db, char const* table, char const* column) {
    Statement stmt;
    std::string sql = Format("PRAGMA table_info(%s)", table);
    if (DB_OK != stmt.Prepare(db, sql.c_str()))
        return -1;
    while (DB_ROW == stmt.Step()) {
        char const* name = stmt.GetValueText(1);
        if (name != nullptr && 0 == strcmp(name, column))
            return stmt.GetValueInt(0);
    }
    return -1;
}

//---------------------------------------------------------------------------------------------
DbResult SeedNullGuids(Db& db, ElementGuidStore& store, SeedCounts& counts, int64_t limit) {
    if (limit == 0)
        return DB_OK;

    // LastMod is left exactly as the real history left it — see ChangesetRewrite.h. The trigger
    // would move it, so it is off for the duration of the pass.
    SuspendTriggers noTriggers(db);

    std::vector<std::pair<uint64_t, double>> rows; // id, and the LastMod it must still have after
    {
        Statement stmt;
        std::string sql = "SELECT Id, LastMod FROM bis_Element WHERE FederationGuid IS NULL ORDER BY Id";
        if (limit > 0)
            sql.append(" LIMIT ").append(std::to_string(limit).c_str());
        DbResult rc = stmt.Prepare(db, sql.c_str());
        if (DB_OK != rc)
            return rc;
        while (DB_ROW == stmt.Step())
            rows.push_back({stmt.GetValueUInt64(0), stmt.GetValueDouble(1)});
    }
    counts.elementsConsidered = (int64_t)rows.size();

    Statement guidate, check;
    DbResult rc = guidate.Prepare(db, "UPDATE bis_Element SET FederationGuid=? WHERE Id=?");
    if (DB_OK != rc)
        return rc;
    rc = check.Prepare(db, "SELECT LastMod FROM bis_Element WHERE Id=?");
    if (DB_OK != rc)
        return rc;

    for (auto const& row : rows) {
        bool previousWasAlive = false;
        uint32_t incarnation = 1;
        Guid guid = store.NewIncarnation(row.first, 0, previousWasAlive, incarnation);
        guidate.Reset();
        guidate.ClearBindings();
        guidate.BindGuid(1, guid);
        guidate.BindUInt64(2, row.first);
        rc = guidate.Step();
        if (DB_DONE != rc)
            return rc;
        ++counts.elementsGuidated;

        // The one thing worth checking: that nothing moved LastMod. A count, not an assumption.
        check.Reset();
        check.ClearBindings();
        check.BindUInt64(1, row.first);
        // A NULL LastMod reads back as 0.0, so ask whether it is null before comparing.
        if (DB_ROW == check.Step() && !check.IsColumnNull(0) &&
            check.GetValueDouble(0) != row.second)
            ++counts.lastModUnstable;
    }
    return DB_OK;
}

} // namespace guidatron
