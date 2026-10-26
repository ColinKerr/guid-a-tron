/*---------------------------------------------------------------------------------------------
 * guid-a-tron — unit tests, on synthetic data.
 *
 * Each test builds its own db with the three things that make `bis_Element` behave like
 * `bis_Element`: an `Id` primary key, a `FederationGuid` blob, and the
 * `bis_Element_CurrentTimeStamp` trigger copied verbatim from a real .bim.
 *
 * No test framework: one CHECK macro, one summary line, non-zero exit if anything failed.
 * Adding gtest to an eight-test file would be the larger dependency.
 *--------------------------------------------------------------------------------------------*/
#include "ChangesetFile.h"
#include "ChangesetId.h"
#include "ChangesetRecords.h"
#include "ChangesetRewrite.h"
#include "Files.h"
#include "GuidATron.h"
#include "Sha1.h"

#include "sqlite3.h"

#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

using namespace guidatron;

namespace {

int g_checks = 0;
int g_failures = 0;
std::string g_test;

#define CHECK(cond, what)                                                                          \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            printf("  FAIL  %s: %s\n", g_test.c_str(), what);                                      \
        }                                                                                          \
    } while (false)

#define CHECK_EQ(actual, expected, what)                                                           \
    do {                                                                                           \
        ++g_checks;                                                                                \
        auto _a = (actual);                                                                        \
        auto _e = (expected);                                                                      \
        if (_a != _e) {                                                                            \
            ++g_failures;                                                                          \
            printf("  FAIL  %s: %s — got %lld, expected %lld\n", g_test.c_str(), what,             \
                   (long long)_a, (long long)_e);                                                  \
        }                                                                                          \
    } while (false)

//! A scratch directory of this test run's own, emptied on first use.
std::string TestDir() {
    static std::string dir;
    if (dir.empty()) {
        dir = TempDir() + "/guid-a-tron-tests/";
        CreateDirectories(dir);
    }
    return dir;
}

std::string Path(char const* name) { return TestDir() + name; }

void Remove(std::string const& path) { RemoveFileIfPresent(path); }

/**
 * A synthetic db with `bis_Element`'s shape and its LastMod trigger.
 *
 * `Id INTEGER PRIMARY KEY` matters: the session extension skips tables with no primary key, so a
 * table without one would track nothing and every test would pass for the wrong reason.
 */
DbResult CreateSyntheticDb(Db& db, std::string const& path, int elements, bool withGuids) {
    Remove(path);
    DbResult rc = db.CreateNewDb(path);
    if (DB_OK != rc)
        return rc;
    rc = db.ExecuteSql("CREATE TABLE bis_Element("
                       "Id INTEGER PRIMARY KEY, FederationGuid BLOB, LastMod REAL, CodeValue TEXT)");
    if (DB_OK != rc)
        return rc;
    // Verbatim from a real .bim's sqlite_master — the reason the seed suspends triggers.
    rc = db.ExecuteSql("CREATE TRIGGER bis_Element_CurrentTimeStamp AFTER UPDATE ON bis_Element "
                       "WHEN old.LastMod=new.LastMod AND old.LastMod!=julianday('now') "
                       "BEGIN UPDATE bis_Element SET LastMod=julianday('now') WHERE Id=new.Id; END");
    if (DB_OK != rc)
        return rc;
    Statement insert;
    rc = insert.Prepare(db, "INSERT INTO bis_Element(Id,FederationGuid,LastMod,CodeValue) VALUES(?,?,?,?)");
    if (DB_OK != rc)
        return rc;
    for (int i = 1; i <= elements; ++i) {
        insert.Reset();
        insert.ClearBindings();
        insert.BindInt64(1, i);
        if (withGuids)
            insert.BindGuid(2, Guid(true));
        else
            insert.BindNull(2);
        insert.BindDouble(3, 2451545.0 + i); // a fixed julian day per row, so LastMod is checkable
        insert.BindText(4, Format("e-%d", i));
        rc = insert.Step();
        if (DB_DONE != rc)
            return rc;
    }
    return db.SaveChanges("synthetic");
}

int64_t ScalarOf(Db& db, char const* sql) {
    Statement stmt;
    if (DB_OK != stmt.Prepare(db, sql))
        return -1;
    return (DB_ROW == stmt.Step()) ? stmt.GetValueInt64(0) : -1;
}

//! The whole of a changeset file, decompressed — which is what SQLite's own session API takes.
bool ReadChangeStream(std::string const& path, std::vector<Byte>& stream) {
    stream.clear();
    ChangesetFileReader reader;
    std::string error;
    if (DB_OK != reader.Open(path, error))
        return false;
    for (;;) {
        size_t had = stream.size();
        stream.resize(had + 64 * 1024);
        int size = 64 * 1024;
        if (DB_OK != reader.Read(stream.data() + had, &size)) {
            stream.resize(had);
            return false;
        }
        stream.resize(had + (size_t)(size < 0 ? 0 : size));
        if (size <= 0)
            return true;
    }
}

//---------------------------------------------------------------------------------------------
// Authoring a changeset, test-only.
//
// The tool itself authors no changesets — it rewrites the ones a real history already pushed — so
// nothing in src/ records changes. The tests need recorded changesets to rewrite, and they come
// from SQLite's session extension: `sqlite3session_changeset` produces the reference encoding of
// the format that ChangesetRecords.h implements, which is what makes these inputs independent of
// the parser under test. The table filter mirrors iTwin's own
// (`TxnManager::_FilterTable`, DgnCore/TxnManager.cpp:1024-1033).
//---------------------------------------------------------------------------------------------
int FilterTable(void*, char const* tableName) {
    auto startsWith = [tableName](char const* prefix) {
        return 0 == strncmp(prefix, tableName, strlen(prefix));
    };
    if (startsWith("be_Local") || startsWith("dgn_Txns") || startsWith("dgn_SpatialIndex") ||
        startsWith("dgn_Rebase") || startsWith("ec_cache_"))
        return 0;
    return 1;
}

/**
 * Record whatever `statements` do as a changeset file, the way the real history recorded its own.
 *
 * The container is this tool's writer with an empty prefix, which is what a changeset carrying no
 * schema change has.
 */
DbResult RecordChangeset(Db& db, std::vector<char const*> const& statements,
                         std::string const& out) {
    sqlite3_session* session = nullptr;
    if (SQLITE_OK != sqlite3session_create(db.GetSqlDb(), "main", &session))
        return DB_ERROR;
    struct Delete {
        sqlite3_session* session;
        ~Delete() { sqlite3session_delete(session); }
    } cleanup{session};

    sqlite3session_table_filter(session, FilterTable, nullptr);
    if (SQLITE_OK != sqlite3session_attach(session, nullptr)) // every table the filter allows
        return DB_ERROR;

    for (char const* sql : statements) {
        DbResult rc = db.ExecuteSql(sql);
        if (DB_OK != rc)
            return rc;
    }
    DbResult rc = db.SaveChanges("recorded");
    if (DB_OK != rc)
        return rc;

    int size = 0;
    void* data = nullptr;
    if (SQLITE_OK != sqlite3session_changeset(session, &size, &data))
        return DB_ERROR;
    struct Free {
        void* data;
        ~Free() { sqlite3_free(data); }
    } freeData{data};

    Remove(out);
    RawChangesetWriter writer(out, std::string());
    rc = writer.Initialize();
    if (DB_OK != rc)
        return rc;
    rc = writer.Append((Byte const*)data, size);
    DbResult finished = writer.Finish();
    return DB_OK != rc ? rc : finished;
}

/**
 * Apply a changeset the way a *consumer* does: the rows, with the file's triggers live.
 *
 */
int ResolveConflict(void*, int cause, sqlite3_changeset_iter*) {
    // REPLACE is only legal for these two causes; anything else has to be omitted or abort.
    return (cause == SQLITE_CHANGESET_DATA || cause == SQLITE_CHANGESET_CONFLICT)
               ? SQLITE_CHANGESET_REPLACE
               : SQLITE_CHANGESET_OMIT;
}

DbResult ApplyLikeAConsumer(Db& db, std::string const& changesetPath) {
    std::vector<Byte> stream;
    if (!ReadChangeStream(changesetPath, stream))
        return DB_ERROR;
    int rc = sqlite3changeset_apply(db.GetSqlDb(), (int)stream.size(), stream.data(), nullptr,
                                    ResolveConflict, nullptr);
    return (DbResult)rc;
}

//! GUIDate a db the way `guid-a-tron seed` does, with a store of the given policy and scope.
DbResult GuidateSeed(Db& db, std::string const& storePath, GuidPolicy policy, GuidCR scope,
                     SeedCounts& counts, int64_t limit = -1) {
    Remove(storePath);
    ElementGuidStore store;
    std::string error;
    DbResult rc = store.Open(storePath, policy, scope, true, error);
    if (DB_OK != rc)
        return rc;
    rc = SeedNullGuids(db, store, counts, limit);
    if (DB_OK == rc)
        rc = store.Flush();
    store.Close();
    return rc;
}

//=============================================================================================
//! What a changeset says about one element, read back with **SQLite's own** changeset iterator
//! rather than with the record parser in src/. That is what makes these assertions a check of the
//! rewrite rather than a check that the parser agrees with itself.
//=============================================================================================
struct ElementChange {
    int op = SQLITE_UPDATE;
    uint64_t id = 0;
    bool oldGuidPresent = false, newGuidPresent = false;
    bool newLastModPresent = false;
    Guid oldGuid, newGuid;
};

//! `lastModColumn` < 0 leaves `newLastModPresent` false.
std::vector<ElementChange> ReadElementChanges(std::string const& csPath, int guidColumn,
                                              int lastModColumn = -1) {
    std::vector<ElementChange> out;
    std::vector<Byte> stream;
    if (!ReadChangeStream(csPath, stream))
        return out;

    sqlite3_changeset_iter* iter = nullptr;
    if (SQLITE_OK != sqlite3changeset_start(&iter, (int)stream.size(), stream.data()))
        return out;

    auto present = [](sqlite3_value* value) {
        return value != nullptr && sqlite3_value_type(value) != SQLITE_NULL;
    };
    auto guidOf = [](sqlite3_value* value) {
        Guid guid;
        if (value != nullptr && sqlite3_value_type(value) == SQLITE_BLOB &&
            sqlite3_value_bytes(value) == (int)sizeof(guid.b))
            memcpy(guid.b, sqlite3_value_blob(value), sizeof(guid.b));
        return guid;
    };

    while (SQLITE_ROW == sqlite3changeset_next(iter)) {
        char const* table = nullptr;
        int columns = 0, op = 0, indirect = 0;
        if (SQLITE_OK != sqlite3changeset_op(iter, &table, &columns, &op, &indirect))
            continue;
        if (table == nullptr || 0 != strcmp(table, "bis_Element"))
            continue;

        ElementChange element;
        element.op = op;
        sqlite3_value* value = nullptr;
        if (op == SQLITE_INSERT)
            sqlite3changeset_new(iter, 0, &value);
        else
            sqlite3changeset_old(iter, 0, &value);
        element.id = value == nullptr ? 0 : (uint64_t)sqlite3_value_int64(value);

        if (op != SQLITE_INSERT) {
            value = nullptr;
            sqlite3changeset_old(iter, guidColumn, &value);
            element.oldGuidPresent = present(value);
            if (element.oldGuidPresent)
                element.oldGuid = guidOf(value);
        }
        if (op != SQLITE_DELETE) {
            value = nullptr;
            sqlite3changeset_new(iter, guidColumn, &value);
            element.newGuidPresent = present(value);
            if (element.newGuidPresent)
                element.newGuid = guidOf(value);
            if (lastModColumn >= 0) {
                value = nullptr;
                sqlite3changeset_new(iter, lastModColumn, &value);
                element.newLastModPresent = present(value);
            }
        }
        out.push_back(element);
    }
    sqlite3changeset_finalize(iter);
    return out;
}

ElementChange const* FindChange(std::vector<ElementChange> const& changes, int op, uint64_t id) {
    for (auto const& change : changes)
        if (change.op == op && change.id == id)
            return &change;
    return nullptr;
}

//! Every change in a changeset, by table and opcode — the census test 4 makes.
void CountChanges(std::string const& csPath, char const* table, int& matching, int& otherTables,
                  int opcode) {
    matching = 0;
    otherTables = 0;
    std::vector<Byte> stream;
    if (!ReadChangeStream(csPath, stream))
        return;
    sqlite3_changeset_iter* iter = nullptr;
    if (SQLITE_OK != sqlite3changeset_start(&iter, (int)stream.size(), stream.data()))
        return;
    while (SQLITE_ROW == sqlite3changeset_next(iter)) {
        char const* name = nullptr;
        int columns = 0, op = 0, indirect = 0;
        if (SQLITE_OK != sqlite3changeset_op(iter, &name, &columns, &op, &indirect))
            continue;
        if (name == nullptr || 0 != strcmp(name, table)) {
            ++otherTables;
            continue;
        }
        if (op == opcode)
            ++matching;
    }
    sqlite3changeset_finalize(iter);
}

//---------------------------------------------------------------------------------------------
// 0. The two primitives this build has to supply itself, because it links nothing from iTwin:
//    SHA-1 (a changeset's id and a derived guid are both defined in terms of it) and the guid's
//    byte-to-string mapping (the value that goes in the column).
//---------------------------------------------------------------------------------------------
void TestSha1AndGuid() {
    g_test = "SHA-1 and the guid agree with the implementations they replace";

    auto hashOf = [](std::string const& text) {
        Sha1 sha;
        sha.Add(text.data(), text.size());
        return sha.GetHashString();
    };
    CHECK(hashOf("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709", "SHA-1 of the empty string");
    CHECK(hashOf("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d", "SHA-1 of \"abc\"");
    CHECK(hashOf("Hello World") == "0a4d55a8d778e5022fab701977c5d840bbc486d0",
          "SHA-1 of \"Hello World\"");
    CHECK(hashOf("The quick brown fox jumps over the lazy dog") ==
              "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12",
          "SHA-1 of the pangram");
    {
        // RFC 3174's third vector: a million 'a'. Also the case that exercises the length field in
        // the padding, which is where a hand-written SHA-1 goes wrong.
        Sha1 sha;
        std::string chunk(1000, 'a');
        for (int i = 0; i < 1000; ++i)
            sha.Add(chunk.data(), chunk.size());
        CHECK(sha.GetHashString() == "34aa973cd4c4daa4f61eeb2bdbad27316534016f",
              "SHA-1 of a million 'a'");
    }
    {
        // Added in awkward pieces, the digest is the same — which is what streaming a changeset
        // through it relies on.
        std::string text = "the-quick-brown-fox-0123456789-abcdefghijklmnopqrstuvwxyz";
        Sha1 whole;
        whole.Add(text.data(), text.size());
        Sha1 pieces;
        for (size_t at = 0; at < text.size(); at += 7)
            pieces.Add(text.data() + at, std::min<size_t>(7, text.size() - at));
        CHECK(whole.GetHashString() == pieces.GetHashString(),
              "the digest does not depend on how the bytes were added");
        // And taking it twice does not consume it: the id generator reads the hash while still
        // holding the stream open.
        CHECK(pieces.GetHashString() == pieces.GetHashString(), "the digest can be taken twice");
    }

    Guid real;
    Byte const bytes[16] = {0xD1, 0x47, 0x4E, 0x09, 0xB8, 0xF3, 0x49, 0x4E,
                            0x9A, 0xC1, 0x96, 0xCB, 0xF5, 0xAA, 0xFF, 0x38};
    memcpy(real.b, bytes, sizeof(bytes));
    CHECK(real.ToString() == "d1474e09-b8f3-494e-9ac1-96cbf5aaff38",
          "a guid's bytes print as the string iTwin printed for the same blob");
    Guid parsed;
    CHECK(parsed.FromString("d1474e09-b8f3-494e-9ac1-96cbf5aaff38"), "and that string parses");
    CHECK(parsed == real, "back to the same sixteen bytes");
    CHECK(parsed.ToString() == "d1474e09-b8f3-494e-9ac1-96cbf5aaff38", "and round-trips");
    CHECK(!Guid().IsValid(), "a zero guid is not valid");
    CHECK(real.IsValid(), "and a real one is");
    CHECK(!parsed.FromString("not-a-guid"), "a string that is not a guid is refused");
    CHECK(!parsed.IsValid(), "and the refusal leaves nothing half-parsed behind");
    CHECK(!parsed.FromString("d1474e09-b8f3-494e-9ac1-96cbf5aaff38-extra"),
          "and so is one with more after it");

    // A generated guid is a version 4, and two of them differ.
    Guid created(true), other(true);
    CHECK(created.IsValid() && created != other, "two generated guids differ");
    CHECK(created.ToString()[14] == '4', "a generated guid is version 4");
    char variant = created.ToString()[19];
    CHECK(variant == '8' || variant == '9' || variant == 'a' || variant == 'b',
          "with the RFC 4122 variant bits");
    // A derived one is a version 5, and is a function of its inputs.
    Guid derivedA = DeriveElementGuid(real, 0x300000dcc6dull, 1);
    CHECK(derivedA.ToString()[14] == '5', "a derived guid is version 5");
    CHECK(derivedA == DeriveElementGuid(real, 0x300000dcc6dull, 1), "and is reproducible");
    CHECK(derivedA != DeriveElementGuid(real, 0x300000dcc6eull, 1), "a different id derives a different guid");
    CHECK(derivedA != DeriveElementGuid(created, 0x300000dcc6dull, 1), "and so does a different scope");
}

//---------------------------------------------------------------------------------------------
// 1. GUIDating a seed fills every guid-less row and moves nothing else — above all not LastMod.
//---------------------------------------------------------------------------------------------
void TestSeedGuidsAndLeavesLastModAlone() {
    g_test = "the seed fills every guid and leaves LastMod exactly as it was";
    Db db;
    std::string path = Path("seed.db");
    CHECK_EQ(CreateSyntheticDb(db, path, 10, false), DB_OK, "create");

    Coverage before = MeasureCoverage(db);
    CHECK_EQ(before.elements, 10, "elements before");
    CHECK_EQ(before.withoutGuid, 10, "without a guid before");

    SeedCounts counts;
    CHECK_EQ(GuidateSeed(db, Path("seed-guids.db"), GuidPolicy::Random, Guid(true), counts),
             DB_OK, "GUIDate the seed");
    CHECK_EQ(counts.elementsGuidated, 10, "GUIDated");
    CHECK_EQ(counts.lastModUnstable, 0, "no row's LastMod moved");

    Coverage after = MeasureCoverage(db);
    CHECK_EQ(after.withoutGuid, 0, "without a guid after");
    CHECK_EQ(after.distinctGuids, 10, "distinct guids");
    // The seed must differ from the baseline in exactly one column. CreateSyntheticDb gave row i
    // LastMod 2451545.0 + i, and every row must still have it.
    CHECK_EQ(ScalarOf(db, "SELECT COUNT(*) FROM bis_Element WHERE LastMod = 2451545.0 + Id"), 10,
             "every row still carries the LastMod the history gave it");
    db.CloseDb();
}

//---------------------------------------------------------------------------------------------
// 2. **Why the triggers have to be suspended**, which is the whole reason the seed is a seed and
//    not an authored guid-only changeset.
//
//    (a) `bis_Element_CurrentTimeStamp` fires even when an UPDATE *does* write LastMod, if it
//        writes the value the row already had — `WHEN old.LastMod=new.LastMod` does not care
//        whether the column was in the SET clause. So a guid-only write cannot keep a row's
//        LastMod by restoring it; the only way is to keep the trigger out of the statement.
//    (b) With the trigger suspended, it keeps it — and the suspension ends with the scope, so the
//        next ordinary write fires the trigger again.
//    (c) So a guid shipped in a changeset of its own does not survive the trip: the trigger fires
//        on the consumer's connection during the apply. The guids belong in the seed.
//---------------------------------------------------------------------------------------------
void TestTriggerAndTheSuspension() {
    g_test = "LastMod can only be kept by keeping the trigger out of the write";

    // (a) Writing LastMod to the value it already holds still fires the trigger.
    {
        Db db;
        CreateSyntheticDb(db, Path("trigger-same-value.db"), 3, false);
        double before = 2451546.0; // row 1: 2451545.0 + 1
        CHECK_EQ(ScalarOf(db, "SELECT COUNT(*) FROM bis_Element WHERE Id=1 AND LastMod=2451546.0"), 1,
                 "row 1 starts at its fixed LastMod");
        {
            Statement stmt;
            stmt.Prepare(db, "UPDATE bis_Element SET FederationGuid=?, LastMod=? WHERE Id=1");
            stmt.BindGuid(1, Guid(true));
            stmt.BindDouble(2, before); // the same value, explicitly in the SET clause
            CHECK_EQ(stmt.Step(), DB_DONE, "update row 1");
        }
        CHECK_EQ(ScalarOf(db, "SELECT COUNT(*) FROM bis_Element WHERE Id=1 AND LastMod=2451546.0"), 0,
                 "the trigger fired anyway: an unchanged LastMod cannot be written");
        db.CloseDb();
    }

    // (b) Suspended, the same write keeps LastMod — and the suspension ends with the scope.
    {
        Db db;
        CreateSyntheticDb(db, Path("trigger-suspended.db"), 3, false);
        {
            SuspendTriggers noTriggers(db);
            Statement stmt;
            stmt.Prepare(db, "UPDATE bis_Element SET FederationGuid=? WHERE Id=1");
            stmt.BindGuid(1, Guid(true));
            CHECK_EQ(stmt.Step(), DB_DONE, "update row 1 with the trigger off");
        }
        CHECK_EQ(ScalarOf(db, "SELECT COUNT(*) FROM bis_Element WHERE Id=1 AND LastMod=2451546.0"), 1,
                 "row 1 kept its LastMod");
        {
            Statement stmt;
            stmt.Prepare(db, "UPDATE bis_Element SET CodeValue='after' WHERE Id=1");
            CHECK_EQ(stmt.Step(), DB_DONE, "an ordinary update afterwards");
        }
        CHECK_EQ(ScalarOf(db, "SELECT COUNT(*) FROM bis_Element WHERE Id=1 AND LastMod=2451546.0"), 0,
                 "and the trigger is live again");
        db.CloseDb();
    }

    // (c) And this is why the guids go in the seed rather than into a changeset of their own.
    //     A guid-only changeset — written by setting the guid and putting LastMod back, so the
    //     session records the guid alone — replays onto a consumer's connection, where the trigger
    //     is live, and every row it touches comes back at julianday('now') instead of the LastMod
    //     the real history gave it. The tool's own apply suspends triggers and so does not show
    //     it; the consumer's does, and the consumer is who the artifact is for.
    std::string guidOnlyCs = Path("guid-only.cs");
    {
        Db db;
        CreateSyntheticDb(db, Path("guid-only.db"), 3, false);
        CHECK_EQ(RecordChangeset(db,
                                 {"UPDATE bis_Element SET FederationGuid = randomblob(16) WHERE Id <= 3",
                                  "UPDATE bis_Element SET LastMod = 2451545.0 + Id WHERE Id <= 3"},
                                 guidOnlyCs),
                 DB_OK, "record a guid-only changeset");
        CHECK_EQ(ScalarOf(db, "SELECT COUNT(*) FROM bis_Element WHERE LastMod = 2451545.0 + Id"), 3,
                 "the author's own rows kept their LastMod");
        db.CloseDb();
    }
    {
        // The session omits a column whose net value did not change, which is what made the
        // restore invisible rather than explicit.
        auto changes = ReadElementChanges(guidOnlyCs, 1 /*FederationGuid*/, 2 /*LastMod*/);
        int carriesGuid = 0, carriesLastMod = 0;
        for (auto const& change : changes) {
            if (change.op != SQLITE_UPDATE)
                continue;
            if (change.newGuidPresent)
                ++carriesGuid;
            if (change.newLastModPresent)
                ++carriesLastMod;
        }
        CHECK_EQ(carriesGuid, 3, "three updates carry a guid");
        CHECK_EQ(carriesLastMod, 0, "and none of them carries LastMod");
    }
    {
        Db replay;
        CreateSyntheticDb(replay, Path("guid-only-consumer.db"), 3, false);
        CHECK_EQ(ApplyLikeAConsumer(replay, guidOnlyCs), DB_OK, "apply it as a consumer would");
        replay.SaveChanges("replayed");
        CHECK_EQ(ScalarOf(replay, "SELECT COUNT(*) FROM bis_Element WHERE FederationGuid IS NULL"), 0,
                 "the guids arrived");
        CHECK_EQ(ScalarOf(replay, "SELECT COUNT(*) FROM bis_Element WHERE LastMod = 2451545.0 + Id"), 0,
                 "but the trigger rewrote every row's LastMod on the way in");
        replay.CloseDb();
    }
    {
        // And the same changeset applied with the trigger suspended keeps LastMod — which is what
        // the seed does to its own rows, and why the guids go there instead of into a changeset.
        Db replay;
        CreateSyntheticDb(replay, Path("guid-only-suspended.db"), 3, false);
        {
            SuspendTriggers noTriggers(replay);
            CHECK_EQ(ApplyLikeAConsumer(replay, guidOnlyCs), DB_OK,
                     "apply it with the trigger suspended");
        }
        replay.SaveChanges("replayed");
        CHECK_EQ(ScalarOf(replay, "SELECT COUNT(*) FROM bis_Element WHERE FederationGuid IS NULL"), 0,
                 "the guids still arrived");
        CHECK_EQ(ScalarOf(replay, "SELECT COUNT(*) FROM bis_Element WHERE LastMod = 2451545.0 + Id"), 3,
                 "and every row kept the LastMod the history gave it");
        replay.CloseDb();
    }
}

//---------------------------------------------------------------------------------------------
// 3. Derived guids are reproducible; random ones are not — and a derived guid is shaped like a
//    UUIDv5, because a consumer reading it should not be able to tell it from a generated one.
//---------------------------------------------------------------------------------------------
void TestDerivedIsReproducible() {
    g_test = "derived guids are stable across rebuilds";
    std::string a = Path("derived-a.db"), b = Path("derived-b.db"), c = Path("random.db");
    // The same derivation scope for both runs, which is what a rebuild of one fork would use. The
    // scope is explicit rather than the file's own DbGuid precisely so this is expressible.
    auto guidsOf = [](std::string const& path, GuidPolicy policy, GuidCR scope) {
        Db db;
        CreateSyntheticDb(db, path, 5, false);
        SeedCounts counts;
        GuidateSeed(db, path + ".guids", policy, scope, counts);
        db.SaveChanges("GUIDated");
        std::string all;
        {
            // Scoped: a Statement still alive when CloseDb() runs takes the process with it.
            Statement stmt;
            stmt.Prepare(db, "SELECT Id, hex(FederationGuid) FROM bis_Element ORDER BY Id");
            while (DB_ROW == stmt.Step()) {
                char const* hex = stmt.GetValueText(1);
                all += std::string(hex == nullptr ? "NULL" : hex) + ",";
            }
        }
        db.CloseDb();
        return all;
    };
    Guid shared(true);
    std::string first = guidsOf(a, GuidPolicy::Derived, shared);
    std::string second = guidsOf(b, GuidPolicy::Derived, shared);
    std::string random = guidsOf(c, GuidPolicy::Random, shared);
    CHECK(!first.empty(), "derived guids were written");
    CHECK(first == second, "two derived runs over the same ids agree");
    CHECK(first != random, "a random run does not agree with a derived one");
    // Shaped like a UUIDv5: version nibble 5, RFC 4122 variant.
    CHECK(first.size() > 20 && first[12] == '5', "version nibble is 5");
}

//---------------------------------------------------------------------------------------------
// 4. A recorded changeset's id is iTwin's id — stable, parent-dependent — and it survives being
//    streamed through a reader and a writer unchanged. That last part is what makes a rewrite
//    possible at all: the id is SHA1 over the parent id and every decompressed byte in order
//    (`DgnCore/ChangesetTxns.cpp:724-772`), so an unchanged id means nothing was lost or moved.
//---------------------------------------------------------------------------------------------
void TestChangesetIdAndRoundTrip() {
    g_test = "a changeset's id is iTwin's id, and a round-trip does not change it";
    Db db;
    std::string path = Path("roundtrip.db");
    CreateSyntheticDb(db, path, 6, false);

    std::string csPath = Path("roundtrip.cs");
    CHECK_EQ(RecordChangeset(db, {"UPDATE bis_Element SET CodeValue='r' WHERE Id <= 6"}, csPath),
             DB_OK, "record six updates");
    db.CloseDb();

    std::string id = ComputeChangesetId(std::string(), csPath);
    CHECK_EQ((int64_t)id.length(), 40, "the id is a 40-character SHA1");
    std::string again = ComputeChangesetId(std::string(), csPath);
    CHECK(again == id, "the id is stable");
    std::string withParent =
        ComputeChangesetId(std::string("a0b1c2d3e4f5a6b7c8d9eaf0a1b2c3d4e5f60718"), csPath);
    CHECK(withParent != id, "the id depends on the parent");

    // And the changeset says what we think it says: six updates to bis_Element, nothing else.
    int updates = 0, otherTables = 0;
    CountChanges(csPath, "bis_Element", updates, otherTables, SQLITE_UPDATE);
    CHECK_EQ(updates, 6, "six element updates");
    CHECK_EQ(otherTables, 0, "no other table is in the changeset");

    // Streaming the file through a reader and a writer, changing nothing, must reproduce the id.
    // This is the premise of the whole tool.
    std::string throughPath = Path("roundtrip-again.cs");
    Remove(throughPath);
    {
        ChangesetFileReader in;
        std::string error;
        CHECK_EQ(in.Open(csPath, error), DB_OK, "open the changeset");
        CHECK(in.Prefix().empty(), "this changeset has no prefix");
        CHECK(!in.ContainsSchemaChanges(), "and so it contains no schema change");
        RawChangesetWriter through(throughPath, in.Prefix());
        CHECK_EQ(through.Initialize(), DB_OK, "initialize the writer");
        CHECK_EQ(through.ReadFrom(in), DB_OK, "stream it through");
        CHECK_EQ(through.Finish(), DB_OK, "finish");
    }
    std::string throughId = ComputeChangesetId(std::string(), throughPath);
    CHECK(throughId == id, "reader -> writer is byte-lossless: the id is unchanged");

    // The whole stream, byte for byte, is what the session extension produced. Nothing above
    // compares the bytes themselves, and that is the one comparison that leaves no room.
    {
        std::vector<Byte> original, copied;
        CHECK(ReadChangeStream(csPath, original), "read the original's stream");
        CHECK(ReadChangeStream(throughPath, copied), "read the copy's stream");
        CHECK(!original.empty() && original == copied,
              "and the decompressed streams are byte-identical");
    }
}

//---------------------------------------------------------------------------------------------
// 5. The record format itself: varints, fields, and the file header this tool writes by hand.
//
//---------------------------------------------------------------------------------------------
void TestRecordFormat() {
    g_test = "the changeset record format";
    using namespace guidatron::records;

    // Varints, at every boundary where SQLite's encoding changes width.
    uint64_t values[] = {0, 1, 16, 127, 128, 255, 16383, 16384, 1u << 20, (1u << 28) - 1, 1u << 28,
                         0x7fffffffu};
    int expectedBytes[] = {1, 1, 1, 1, 2, 2, 2, 3, 3, 4, 5, 5};
    int at = 0;
    for (uint64_t value : values) {
        Byte buffer[10];
        int wrote = PutVarint(buffer, value);
        CHECK_EQ(wrote, expectedBytes[at], "the varint is the width SQLite would use");
        uint64_t read = 0;
        int consumed = GetVarint(buffer, (size_t)wrote, read);
        CHECK_EQ(consumed, wrote, "reading it consumes what writing it produced");
        CHECK_EQ((int64_t)read, (int64_t)value, "the varint round-trips");
        // Truncated by one byte, the reader must ask for more rather than guess.
        if (wrote > 1)
            CHECK_EQ(GetVarint(buffer, (size_t)wrote - 1, read), 0, "a short varint is incomplete");
        ++at;
    }

    // Fields. A NULL and an undefined are one byte each and are *not* the same thing: an undefined
    // guid in an update's old record means "not modified", a NULL means "it was NULL".
    Byte nullField[] = {TypeNull};
    Field field;
    CHECK(ParseField(nullField, 1, field) == Parse::Ok && field.IsNull() && !field.IsUndefined(),
          "a NULL field");
    Byte undefinedField[] = {TypeUndefined};
    CHECK(ParseField(undefinedField, 1, field) == Parse::Ok && field.IsUndefined() && !field.IsNull(),
          "an undefined field");
    // 0x300000dcc6d, the element id that is inserted, deleted and then inserted again.
    Byte intField[] = {TypeInt, 0x00, 0x00, 0x03, 0x00, 0x00, 0x0d, 0xcc, 0x6d};
    CHECK(ParseField(intField, sizeof(intField), field) == Parse::Ok, "an integer field");
    CHECK_EQ((int64_t)field.AsUInt64(), (int64_t)0x300000dcc6dull, "big-endian, eight bytes");
    CHECK_EQ(ParseField(intField, 5, field), Parse::NeedMore, "a short integer field");
    Byte badField[] = {0x09};
    CHECK_EQ(ParseField(badField, 1, field), Parse::Malformed, "an undefined type byte");

    // A blob field written by this tool, read back by this tool.
    Guid guid(true);
    std::vector<Byte> blob;
    AppendBlobField(blob, &guid, (int)sizeof(guid));
    CHECK_EQ((int64_t)blob.size(), 18, "type byte, one-byte varint length, sixteen bytes");
    CHECK(ParseField(blob.data(), blob.size(), field) == Parse::Ok &&
              field.IsBlobOf((int)sizeof(Guid)),
          "it reads back as a 16-byte blob");
    CHECK(0 == memcmp(field.payload, &guid, sizeof(guid)), "and holds the guid's own bytes");

    // A table header, with the PK layout the rewriter checks.
    Byte header[] = {TableLead, 4, 1, 0, 0, 0, 'b', 'i', 's', '_', 'E', 'l', 'e', 'm', 'e', 'n', 't', 0};
    TableHeader table;
    CHECK(ParseTableHeader(header, sizeof(header), table) == Parse::Ok, "a table header");
    CHECK_EQ(table.nCol, 4, "four columns");
    CHECK(table.name == "bis_Element", "the table's name");
    CHECK(table.IsOnlyPkColumn(0), "Id is the one primary key column");
    CHECK(!table.IsOnlyPkColumn(1), "and the guid column is not");
    CHECK_EQ(ParseTableHeader(header, sizeof(header) - 1, table), Parse::NeedMore,
             "a name whose nul has not arrived yet");
}

//---------------------------------------------------------------------------------------------
// 6. The file container this tool writes, which exists because iTwin's own writer rebuilds the
//    prefix instead of carrying it: `ChangesetFileWriter` constructs a fresh JSON document and
//    forces ContainsSchemaChanges true whenever there is DDL, so a schema changeset round-tripped
//    through it comes out with a different id.
//---------------------------------------------------------------------------------------------
void TestRawWriterCarriesThePrefix() {
    g_test = "the raw writer carries the prefix's bytes";
    // A prefix iTwin's writer would never produce: the keys in the other order, and
    // ContainsSchemaChanges false while there is DDL. The point is that this writer reproduces
    // whatever it is given, including a combination that cannot be rebuilt from its meaning.
    std::string prefix("{\"DDL\":\"CREATE TABLE zzz(a)\",\"ContainsSchemaChanges\":false}");
    std::string csPath = Path("raw-prefix.cs");
    Remove(csPath);

    // Something to put in the stream: one table header and one insert, hand-built.
    std::vector<Byte> stream;
    Byte header[] = {guidatron::records::TableLead, 2, 1, 0, 'z', 'z', 'z', 0};
    stream.insert(stream.end(), header, header + sizeof(header));
    stream.push_back(guidatron::records::OpInsert);
    stream.push_back(0);
    Byte id[] = {guidatron::records::TypeInt, 0, 0, 0, 0, 0, 0, 0, 7};
    stream.insert(stream.end(), id, id + sizeof(id));
    stream.push_back(guidatron::records::TypeNull);
    {
        RawChangesetWriter writer(csPath, prefix);
        CHECK_EQ(writer.Initialize(), DB_OK, "initialize");
        CHECK_EQ(writer.Append(stream.data(), (int)stream.size()), DB_OK, "append");
        CHECK_EQ(writer.Finish(), DB_OK, "finish");
    }

    // The 22-byte header, byte for byte as a real changeset carries it.
    Byte expected[] = {0x16, 0x00, 'C', 'h',  'a',  'n',  'g',  'e', 'S',
                       'e',  't',  'L', 'z',  'm',  'a',  0x00, 0x00, 0x00,
                       0x10, 0x00, 0x02, 0x00};
    Byte actual[22] = {0};
    if (FILE* file = fopen(csPath.c_str(), "rb")) {
        size_t read = fread(actual, 1, sizeof(actual), file);
        fclose(file);
        CHECK_EQ((int64_t)read, 22, "the header is 22 bytes");
    } else {
        CHECK(false, "the file this tool wrote can be opened");
    }
    CHECK(0 == memcmp(actual, expected, sizeof(expected)), "and it is the header iTwin expects");

    // Read back: the prefix comes out byte for byte, including the flag iTwin's writer would have
    // forced to true, and the change stream is the one that went in.
    {
        ChangesetFileReader reader;
        std::string error;
        CHECK_EQ(reader.Open(csPath, error), DB_OK, "the file reads back");
        CHECK(reader.Prefix() == prefix, "the prefix comes back out byte for byte");
        CHECK(!reader.ContainsSchemaChanges(),
              "the original's ContainsSchemaChanges survived, false and all");
        std::vector<Byte> back;
        CHECK(ReadChangeStream(csPath, back), "the change stream reads back");
        CHECK(back == stream, "and it is byte-identical to what was appended");
    }

    // A prefix that does say so is read as saying so — the flag is the prefix's, not a default.
    {
        std::string schemaPath = Path("raw-prefix-schema.cs");
        Remove(schemaPath);
        RawChangesetWriter writer(schemaPath,
                                  std::string("{\"ContainsSchemaChanges\":true,\"DDL\":\"CREATE TABLE q(a)\"}"));
        writer.Initialize();
        writer.Append(stream.data(), (int)stream.size());
        writer.Finish();
        ChangesetFileReader reader;
        std::string error;
        CHECK_EQ(reader.Open(schemaPath, error), DB_OK, "the schema changeset reads back");
        CHECK(reader.ContainsSchemaChanges(), "and it reports a schema change");
    }

    // And the id: a no-op copy through this tool keeps it, prefix and all. (That the *other*
    // writer does not is what this class exists for; it is no longer linked, so the property is
    // checked where it is now checkable
    std::string before = ComputeChangesetId(std::string(), csPath);
    CHECK_EQ((int64_t)before.length(), 40, "the id is a 40-character SHA1");
    std::string again = Path("raw-prefix-again.cs");
    Remove(again);
    {
        ChangesetFileReader source;
        std::string error;
        CHECK_EQ(source.Open(csPath, error), DB_OK, "reopen it");
        RawChangesetWriter writer(again, source.Prefix());
        writer.Initialize();
        writer.ReadFrom(source);
        writer.Finish();
    }
    CHECK(before == ComputeChangesetId(std::string(), again),
          "a no-op copy through this tool keeps the changeset's id");

    // An empty prefix is a length of 0 and no bytes, which is what nearly every changeset has.
    {
        std::string emptyPath = Path("raw-prefix-empty.cs");
        Remove(emptyPath);
        RawChangesetWriter writer(emptyPath, std::string());
        writer.Initialize();
        writer.Append(stream.data(), (int)stream.size());
        writer.Finish();
        ChangesetFileReader reader;
        std::string error;
        CHECK_EQ(reader.Open(emptyPath, error), DB_OK, "a prefixless changeset reads back");
        CHECK(reader.Prefix().empty(), "with no prefix");
        CHECK(ComputeChangesetId(std::string(), emptyPath) != before,
              "and a different id, because the prefix counts for the id");
    }

    // Something that is not a changeset at all is refused rather than misread.
    {
        std::string junkPath = Path("not-a-changeset.cs");
        Remove(junkPath);
        if (FILE* file = fopen(junkPath.c_str(), "wb")) {
            char const* junk = "this is not a changeset, it is a sentence";
            fwrite(junk, 1, strlen(junk), file);
            fclose(file);
        }
        ChangesetFileReader reader;
        std::string error;
        CHECK_EQ(reader.Open(junkPath, error), DB_ERROR, "a file that is not a changeset is refused");
        CHECK(!error.empty(), "and the refusal says why");
    }
}

//---------------------------------------------------------------------------------------------
// 7. End to end: the seed, the rewrite, a deleted row's old value, and a reused id.
//
//    The shape is the real one. A "history" db that never has a guid, the changesets a connector
//    would have recorded on it, and a "seed" taken from the same baseline and GUIDated — then the
//    changesets rewritten against that seed's state. The synthetic bis_Element puts FederationGuid
//    at column 1 rather than 10, which is worth having: the column's ordinal is configuration, and
//    a test that only ever saw 10 would not notice if it were hard-wired.
//---------------------------------------------------------------------------------------------
void TestRewriteWithState() {
    g_test = "rewriting guids into changesets, old values included";
    std::string historyPath = Path("n-history.db");
    std::string seedPath = Path("n-seed.db");
    std::string storePath = Path("n-guids.db");
    Remove(storePath);
    Remove(seedPath);

    // The baseline, with no guids anywhere — which is every real model this study has.
    {
        Db history;
        CHECK_EQ(CreateSyntheticDb(history, historyPath, 3, false), DB_OK, "the baseline");
        CHECK_EQ(FindColumnOrdinal(history, "bis_Element", "FederationGuid"), 1,
                 "the guid column's ordinal comes from the file, not from a constant");
        history.CloseDb();
    }
    std::string copyError;
    CHECK(CopyFileFailIfPresent(historyPath, seedPath, copyError), "copy the baseline to a seed");

    RewriteOptions options;
    options.guidColumn = 1;
    Guid scope(true);
    Guid seedGuidOfElement2;

    // The seed: a guid on all three, and LastMod exactly as the baseline left it.
    {
        Db seed;
        CHECK_EQ(seed.OpenDb(seedPath, Db::OpenMode::ReadWrite), DB_OK, "open the seed");
        ElementGuidStore store;
        std::string error;
        CHECK_EQ(store.Open(storePath, GuidPolicy::Random, scope, true, error), DB_OK,
                 "create the guid store");
        SeedCounts counts;
        CHECK_EQ(SeedNullGuids(seed, store, counts, -1), DB_OK, "GUIDate the seed");
        CHECK_EQ(counts.elementsGuidated, 3, "three elements GUIDated");
        CHECK_EQ(counts.lastModUnstable, 0, "and LastMod did not move");
        CHECK_EQ(MeasureCoverage(seed).withoutGuid, 0, "the seed has no guid-less element left");
        // LastMod is the history's own, so no later original update can arrive with an old
        // LastMod that no longer matches the row it lands on.
        CHECK_EQ(ScalarOf(seed, "SELECT COUNT(*) FROM bis_Element WHERE LastMod = 2451545.0 + Id"), 3,
                 "the seed differs from the baseline in exactly one column");
        CHECK(store.Lookup(2, seedGuidOfElement2), "the store remembers element 2's guid");
        CHECK_EQ((int64_t)store.Size(), 3, "one entry per GUIDated element");
        seed.SaveChanges("seeded");
        seed.CloseDb();
        store.Close();
    }

    // The original history moves on: element 2 is deleted, element 10 is created without a guid,
    // and element 3 is edited.
    std::string csA = Path("n-cs-a.cs");
    {
        Db history;
        CHECK_EQ(history.OpenDb(historyPath, Db::OpenMode::ReadWrite), DB_OK, "reopen the history");
        CHECK_EQ(RecordChangeset(history,
                                 {"DELETE FROM bis_Element WHERE Id=2",
                                  "INSERT INTO bis_Element(Id,FederationGuid,LastMod,CodeValue)"
                                  " VALUES(10,NULL,2451600.0,'e-10')",
                                  "UPDATE bis_Element SET CodeValue='edited', LastMod=2451601.0"
                                  " WHERE Id=3"},
                                 csA),
                 DB_OK, "record the original changeset");
        history.CloseDb();
    }

    // What the original says, before anything is rewritten: the delete carries a NULL old guid, and
    // that is the value a transformer reads to propagate the deletion.
    {
        auto changes = ReadElementChanges(csA, options.guidColumn);
        ElementChange const* deleted = FindChange(changes, SQLITE_DELETE, 2);
        CHECK(deleted != nullptr && !deleted->oldGuidPresent,
              "the original's delete records no guid for the deleted element");
        ElementChange const* inserted = FindChange(changes, SQLITE_INSERT, 10);
        CHECK(inserted != nullptr && !inserted->newGuidPresent,
              "and its insert brings none either");
    }

    // A scan: the census, and the round-trip. Nothing is substituted and nothing is recorded, so
    // the id must come back identical — that is this tool's own test of the format.
    {
        ElementGuidStore unused;
        RewriteOptions scan = options;
        scan.substitute = false;
        RewriteCounts counts;
        std::string rewrittenId;
        std::string error;
        CHECK_EQ(RewrittenChangesetId(csA, std::string(), unused, scan, counts, rewrittenId, error),
                 DB_OK, "scan the changeset");
        CHECK(rewrittenId == ComputeChangesetId(std::string(), csA),
              "a rewrite that substitutes nothing reproduces the changeset's id exactly");
        CHECK_EQ(counts.insertsWithoutGuid, 1, "one insert without a guid");
        CHECK_EQ(counts.deletesWithoutGuid, 1, "one delete whose old guid is NULL");
        CHECK_EQ(counts.updateOldsWithoutGuid, 0, "the update does not touch the guid");
        CHECK_EQ(counts.fieldsWritten, 0, "and a scan writes nothing");
    }

    // An empty store cannot fill the delete, and says so rather than inventing a guid. This is the
    // state of affairs a fork started in the middle of a history would be in.
    {
        std::string emptyStorePath = Path("n-guids-empty.db");
        Remove(emptyStorePath);
        ElementGuidStore empty;
        std::string error;
        CHECK_EQ(empty.Open(emptyStorePath, GuidPolicy::Random, scope, true, error), DB_OK,
                 "an empty store");
        RewriteCounts counts;
        std::string out = Path("n-cs-a-nostate.cs");
        Remove(out);
        CHECK_EQ(RewriteChangeset(csA, out, empty, options, counts, error), DB_OK,
                 "rewrite with no state");
        CHECK_EQ(counts.unresolved, 1, "the delete's guid is reported unresolved, never guessed");
        CHECK_EQ(counts.fieldsWritten, 1, "only the insert could be filled");
        empty.Close();
    }

    // The rewrite proper.
    std::string csARewritten = Path("n-cs-a-rewritten.cs");
    Guid firstGuidOfElement10;
    {
        ElementGuidStore store;
        std::string error;
        CHECK_EQ(store.Open(storePath, GuidPolicy::Random, scope, false, error), DB_OK,
                 "reopen the store the seed created");
        CHECK_EQ((int64_t)store.Size(), 3, "the store survived being closed");
        Guid reloaded;
        CHECK(store.Lookup(2, reloaded) && reloaded == seedGuidOfElement2,
              "and element 2's guid came back the same");

        RewriteCounts counts;
        options.index = 2;
        Remove(csARewritten);
        CHECK_EQ(RewriteChangeset(csA, csARewritten, store, options, counts, error), DB_OK,
                 "rewrite the changeset");
        CHECK_EQ(counts.fieldsWritten, 2, "two guid fields written: the insert and the delete");
        CHECK_EQ(counts.unresolved, 0, "nothing was left unresolved");
        CHECK_EQ(counts.guidsAssigned, 1, "one new element needed a guid");
        CHECK_EQ(counts.newIncarnations, 0, "and it is the first element to use that id");
        CHECK(store.Lookup(10, firstGuidOfElement10), "element 10 is in the store now");
        store.Close();
    }

    // The rewritten changeset, read back by BeSQLite: the delete now carries the guid the seed gave
    // element 2 — the value an `advance` cannot put there, and the one the transformer reads.
    {
        auto changes = ReadElementChanges(csARewritten, options.guidColumn);
        ElementChange const* deleted = FindChange(changes, SQLITE_DELETE, 2);
        CHECK(deleted != nullptr && deleted->oldGuidPresent,
              "the rewritten delete carries a guid in its old values");
        CHECK(deleted != nullptr && deleted->oldGuid == seedGuidOfElement2,
              "and it is the guid the element actually has in the seed");
        ElementChange const* inserted = FindChange(changes, SQLITE_INSERT, 10);
        CHECK(inserted != nullptr && inserted->newGuidPresent, "the insert carries one too");
        CHECK(inserted != nullptr && inserted->newGuid == firstGuidOfElement10,
              "and it is the one the store recorded");
        ElementChange const* updated = FindChange(changes, SQLITE_UPDATE, 3);
        CHECK(updated != nullptr && !updated->oldGuidPresent && !updated->newGuidPresent,
              "the update is untouched: it never mentioned the guid");
    }

    // And it applies. A consumer takes the seed and the rewritten changeset and ends up with a file
    // where every element has a guid — with the triggers live, because this is the consumer's path.
    {
        Db consumer;
        CHECK_EQ(consumer.OpenDb(seedPath, Db::OpenMode::ReadWrite), DB_OK,
                 "open the seed as a consumer");
        CHECK_EQ(ApplyLikeAConsumer(consumer, csARewritten), DB_OK,
                 "apply the rewritten changeset");
        consumer.SaveChanges("applied");
        CHECK_EQ(MeasureCoverage(consumer).withoutGuid, 0,
                 "every element in the result has a guid");
        CHECK_EQ(ScalarOf(consumer, "SELECT COUNT(*) FROM bis_Element WHERE Id=2"), 0,
                 "and the deleted element is gone");
        CHECK_EQ(ScalarOf(consumer, "SELECT COUNT(*) FROM bis_Element WHERE Id=10"), 1,
                 "and the new one arrived");
        consumer.CloseDb();
    }

    // The reused id: the original deletes element 10 and later inserts the same id again.
    std::string csB = Path("n-cs-b.cs"), csC = Path("n-cs-c.cs");
    {
        Db history;
        history.OpenDb(historyPath, Db::OpenMode::ReadWrite);
        CHECK_EQ(RecordChangeset(history, {"DELETE FROM bis_Element WHERE Id=10"}, csB),
                 DB_OK, "record the delete");
        CHECK_EQ(RecordChangeset(history,
                                 {"INSERT INTO bis_Element(Id,FederationGuid,LastMod,CodeValue)"
                                  " VALUES(10,NULL,2451700.0,'e-10 again')"},
                                 csC),
                 DB_OK, "record the re-insert of the same id");
        history.CloseDb();
    }
    {
        ElementGuidStore store;
        std::string error;
        store.Open(storePath, GuidPolicy::Random, scope, false, error);
        std::string outB = Path("n-cs-b-rewritten.cs"), outC = Path("n-cs-c-rewritten.cs");
        Remove(outB);
        Remove(outC);
        RewriteCounts countsB, countsC;
        options.index = 3;
        CHECK_EQ(RewriteChangeset(csB, outB, store, options, countsB, error), DB_OK,
                 "rewrite the delete");
        CHECK_EQ(countsB.deletesWithoutGuid, 1, "the delete of element 10 had no guid either");
        CHECK_EQ(countsB.fieldsWritten, 1, "and the store could fill it");
        options.index = 4;
        CHECK_EQ(RewriteChangeset(csC, outC, store, options, countsC, error), DB_OK,
                 "rewrite the re-insert");
        CHECK_EQ(countsC.newIncarnations, 1, "the re-inserted id is a second incarnation");
        CHECK_EQ((int64_t)store.Retired(), 1, "and the first incarnation's guid was retired");

        auto deleteChanges = ReadElementChanges(outB, options.guidColumn);
        ElementChange const* deleted = FindChange(deleteChanges, SQLITE_DELETE, 10);
        CHECK(deleted != nullptr && deleted->oldGuid == firstGuidOfElement10,
              "the delete carries the guid the first element 10 held");
        auto insertChanges = ReadElementChanges(outC, options.guidColumn);
        ElementChange const* inserted = FindChange(insertChanges, SQLITE_INSERT, 10);
        CHECK(inserted != nullptr && inserted->newGuidPresent, "the re-insert carries a guid");
        CHECK(inserted != nullptr && inserted->newGuid != firstGuidOfElement10,
              "and it is NOT the dead element's guid: a reused id is not the same element");
        Guid current;
        CHECK(store.Lookup(10, current) && current == inserted->newGuid,
              "the store now holds the living incarnation's guid");
        store.Close();
    }

    // Derived guids, which exist so a fork can be rebuilt rather than archived, have to survive
    // reuse too: the incarnation is part of the derivation.
    g_test = "derived guids distinguish incarnations";
    Guid first = DeriveElementGuid(scope, 0x300000dcc6dull, 1);
    Guid second = DeriveElementGuid(scope, 0x300000dcc6dull, 2);
    CHECK(first != second, "two incarnations of one id derive two different guids");
    CHECK(first == DeriveElementGuid(scope, 0x300000dcc6dull, 1), "and the first is reproducible");
}

//---------------------------------------------------------------------------------------------
// 8. The one check the synthetic tests cannot make: a changeset **iTwin wrote**.
//
//    Everything above authors its inputs with SQLite's session extension and wraps them in this
//    tool's own container, so the container is checked only against itself. Pointed at a directory
//    of real changesets named `<index>-<id>.cs`.
//    This test closes that loop three ways on files produced by iTwin and ids assigned by
//    iModelHub:
//
//      - **the id**: each changeset's id, computed from its parent's id and its own bytes, must be
//        the id in its filename. That is iTwin's arithmetic, iTwin's prefix handling and iTwin's
//        LZMA2 stream, against this build's.
//      - **the container**: the real file's header and prefix are read by this tool's reader.
//      - **the round-trip**: copied through this tool's writer with nothing changed, the
//        decompressed stream is identical and so is the id — including for a changeset that
//        carries a schema prefix, which is the case iTwin's own writer cannot reproduce.
//
//    Skipped, loudly, when there is no such directory to hand, because real changesets are not in
//    this repository and never will be.
//---------------------------------------------------------------------------------------------
void TestRealChangeset() {
    g_test = "changesets iTwin wrote";
    char const* dir = getenv("GUID_A_TRON_REAL_CHANGESETS");
    if (dir == nullptr || !FileExists(dir)) {
        printf("        skipped: set GUID_A_TRON_REAL_CHANGESETS to a directory of\n"
               "        <index>-<id>.cs files to check against changesets iTwin wrote\n");
        return;
    }

    struct Real {
        int64_t index = 0;
        std::string id;
        std::string path;
    };
    std::vector<Real> files;
    for (std::string const& path : ListFiles(dir, ".cs")) {
        std::string name = BaseName(path);
        // <index>-<40 hex>.cs.
        size_t dash = name.find('-');
        if (dash == std::string::npos || name.size() != dash + 1 + 40 + 3)
            continue;
        Real real;
        real.index = atoll(name.substr(0, dash).c_str());
        real.id = name.substr(dash + 1, 40);
        real.path = path;
        files.push_back(real);
    }
    std::sort(files.begin(), files.end(),
              [](Real const& a, Real const& b) { return a.index < b.index; });
    CHECK(files.size() >= 2, "at least two consecutive changesets to chain");
    if (files.size() < 2)
        return;

    int64_t limit = 4;
    if (char const* asked = getenv("GUID_A_TRON_REAL_LIMIT"))
        limit = atoll(asked);

    int64_t checked = 0, roundTripped = 0, withPrefix = 0;
    for (size_t i = 1; i < files.size() && (limit <= 0 || checked < limit); ++i) {
        Real const& parent = files[i - 1];
        Real const& child = files[i];
        if (child.index != parent.index + 1)
            continue; // not consecutive: the parent of this one is not in the directory

        // The id, computed the way iTwin computes it, against the id iModelHub assigned.
        std::string computed = ComputeChangesetId(parent.id, child.path);
        CHECK(computed == child.id, "a real changeset's id is the id in its filename");
        if (computed != child.id)
            printf("        %s: computed %s\n", BaseName(child.path).c_str(), computed.c_str());
        ++checked;

        // The container, and the round-trip.
        ChangesetFileReader reader;
        std::string error;
        if (DB_OK != reader.Open(child.path, error)) {
            CHECK(false, "a real changeset opens");
            printf("        %s\n", error.c_str());
            continue;
        }
        if (!reader.Prefix().empty())
            ++withPrefix;

        std::string copy = Path("real-copy.cs");
        Remove(copy);
        {
            RawChangesetWriter writer(copy, reader.Prefix());
            CHECK_EQ(writer.Initialize(), DB_OK, "start a copy of it");
            CHECK_EQ(writer.ReadFrom(reader), DB_OK, "stream it through unchanged");
            CHECK_EQ(writer.Finish(), DB_OK, "finish the copy");
        }
        CHECK(ComputeChangesetId(parent.id, copy) == child.id,
              "a no-op copy of a real changeset keeps the id iModelHub assigned");
        std::vector<Byte> original, copied;
        CHECK(ReadChangeStream(child.path, original), "read the real stream");
        CHECK(ReadChangeStream(copy, copied), "read the copy's stream");
        CHECK(!original.empty() && original == copied,
              "and the decompressed streams are byte-identical");
        Remove(copy);
        ++roundTripped;
    }
    printf("        %lld id(s) against iModelHub's own, %lld round-tripped, %lld with a prefix\n",
           (long long)checked, (long long)roundTripped, (long long)withPrefix);
    CHECK(checked > 0, "at least one real changeset was checked");
}

} // namespace

int main(int, char**) {
    if (!InitializeSqlite())
        return 1;
    setvbuf(stdout, nullptr, _IONBF, 0); // a crash must not take the output with it
    printf("guid-a-tron tests (synthetic data)\n");
    struct { char const* name; void (*fn)(); } tests[] = {
        {"SHA-1 and the guid", TestSha1AndGuid},
        {"the seed GUIDates and leaves LastMod alone", TestSeedGuidsAndLeavesLastModAlone},
        {"the trigger and the suspension", TestTriggerAndTheSuspension},
        {"derived guids are stable", TestDerivedIsReproducible},
        {"the changeset id and the round-trip", TestChangesetIdAndRoundTrip},
        {"the changeset record format", TestRecordFormat},
        {"the raw writer carries the prefix", TestRawWriterCarriesThePrefix},
        {"rewriting guids into changesets, with state", TestRewriteWithState},
        {"changesets iTwin wrote", TestRealChangeset},
    };
    for (auto const& test : tests) {
        printf("  %s\n", test.name);
        test.fn();
    }
    printf("%d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
