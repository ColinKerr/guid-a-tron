/*---------------------------------------------------------------------------------------------
 * guid-a-tron — rewriting a changeset's `FederationGuid` column, in the original's own byte order.
 *
 * `FederationGuid` is one column of one table, so a history can be given element guids by editing
 * that column wherever it is NULL and leaving every other byte alone:
 *
 *     original changeset --(decompress)--> change stream --(substitute)--> change stream --> .cs
 *
 * The alternative — applying each original changeset to a briefcase, writing guids onto whatever
 * arrived without one, and re-authoring the original's rows plus those writes as one changeset —
 * cannot do the thing that matters most: **a deleted element's guid lives in the change's _old_
 * values**, and appending rows to a changeset can only add new values. A transformer reads a
 * deletion's guid out of `row.oldValues` (`core-backend/ChangesetReader.js:128-152`,
 * `stage: "Old"`), so a deletion whose old `FederationGuid` is NULL is a deletion that cannot be
 * propagated by guid. Rewriting reaches those bytes; re-authoring does not.
 *
 * So the rewriter keeps an `ElementGuidStore`: one row per element id this tool has given a guid
 * to, carrying the guid, whether the element is alive, and which incarnation it is. A delete reads
 * it; an insert of an id already in it starts a new incarnation and retires the old guid. The
 * store is a file, so a long run can stop and resume, and so the guids are auditable after the
 * fact rather than only re-derivable.
 *
 * What the rewriter does **not** do: it never applies anything, opens no briefcase, needs no ECDb,
 * runs no DDL, suspends no triggers, and cannot leave a half-applied file behind. The deliverable
 * is the GUIDated **seed** (the baseline with its guid-less elements filled in, `LastMod`
 * untouched) plus the rewritten changesets. See ../IMPLEMENTATION.md.
 *--------------------------------------------------------------------------------------------*/
#pragma once

#include "ChangesetFile.h"
#include "GuidATron.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace guidatron {

//! What the store knows about one element id now.
struct GuidRecord {
    Guid guid;
    uint32_t incarnation = 1; //!< 1 for the first element to use this id, 2 for the next, …
    bool alive = true;        //!< false once a delete has been seen for this incarnation
    //! false when the guid is the original's own, read out of a changeset rather than chosen here.
    bool assigned = true;
    int64_t sinceIndex = 0; //!< the changeset index this incarnation appeared at (0 = the seed)
};

/**
 * `elementId -> guid`, persisted, with one entry per incarnation of an id.
 *
 * Held in memory for the whole run (an `unordered_map`, ~40 bytes an entry) and flushed
 * to a SQLite file after each changeset, so the run is resumable and the result is auditable.
 *
 * Only elements this tool has **given** a guid to need an entry, which is why the store is a
 * fraction of the corpus: an element whose insert already carried a guid carries it in the delete's
 * old values too, so there is nothing to look up. Entries that exist are still updated when the
 * original's own bytes say something about them, so `alive` stays honest and reuse is detectable.
 */
struct ElementGuidStore {
    ElementGuidStore() {}
    ~ElementGuidStore() { Close(); }
    ElementGuidStore(ElementGuidStore const&) = delete;
    ElementGuidStore& operator=(ElementGuidStore const&) = delete;

    /**
     * Open or create the store. The policy and scope are written on creation.
     *
     * On reopen the policy is the **store's**, not the caller's: a run that mixed random and
     * derived guids would be neither reproducible nor explicable. `policyIsExplicit` says whether
     * the caller asked for one — if it did and the store disagrees, the open fails rather than
     * quietly doing something else.
     */
    DbResult Open(std::string const& path, GuidPolicy policy, GuidCR scope, bool createIfMissing,
                  std::string& error, bool policyIsExplicit = true);
    bool IsOpen() const { return m_db.IsDbOpen(); }
    //! Write everything this run has changed, in one transaction. Called after each changeset.
    DbResult Flush();
    void Close();

    //! The guid this id's current incarnation holds, if this tool gave it one.
    bool Lookup(uint64_t elementId, Guid& guid) const;

    /**
     * Give `elementId` a guid for a new incarnation, retiring any guid it held before.
     *
     * `previousWasAlive` reports the anomaly worth knowing about: an insert of an id the store
     * still believes is alive, which means a delete went unseen.
     */
    Guid NewIncarnation(uint64_t elementId, int64_t index, bool& previousWasAlive,
                          uint32_t& incarnation);
    //! The original's own guid for an id already in the store: adopt it, and revive the entry.
    void Observe(uint64_t elementId, GuidCR guid, int64_t index);
    void MarkDeleted(uint64_t elementId, int64_t index);

    size_t Size() const { return m_entries.size(); }
    int64_t Assigned() const { return m_assigned; }
    int64_t Retired() const { return m_retired; }
    GuidPolicy Policy() const { return m_policy; }
    Guid Scope() const { return m_scope; }

    bool GetMeta(char const* name, std::string& value) const;
    DbResult SetMeta(char const* name, std::string const& value);
    //! The highest changeset index this store has been advanced through, or 0.
    int64_t LastIndex() const;
    std::string LastChangesetId() const;
    DbResult RecordProgress(int64_t index, std::string const& changesetId);

private:
    struct RetiredRow {
        uint64_t elementId;
        uint32_t incarnation;
        Guid guid;
        int64_t sinceIndex;
        int64_t retiredIndex;
    };
    DbResult CreateTables();
    DbResult LoadEntries();
    Guid ChooseGuid(uint64_t elementId, uint32_t incarnation) const;

    Db m_db;
    std::unordered_map<uint64_t, GuidRecord> m_entries;
    std::unordered_set<uint64_t> m_dirty;
    std::vector<RetiredRow> m_pendingRetired;
    GuidPolicy m_policy = GuidPolicy::Random;
    Guid m_scope;
    int64_t m_assigned = 0;
    int64_t m_retired = 0;
};

//! Where the guid lives, and which changeset is being rewritten.
struct RewriteOptions {
    std::string table = "bis_Element";
    //! `FederationGuid`'s ordinal in `bis_Element`, which a changeset's table header does not carry
    //! — a changeset names columns by position only. Read from the seed's own
    //! `PRAGMA table_info(bis_Element)` rather than assumed; 10 are 
    //! checked (Id, ECClassId, ModelId, LastMod, CodeSpecId, CodeScopeId, CodeValue, UserLabel,
    //! ParentId, ParentRelECClassId, **FederationGuid**, JsonProperties).
    int guidColumn = 10;
    int idColumn = 0;       //!< `Id`, the table's primary key
    int64_t index = 0;      //!< the changeset's index in the timeline, recorded in the store
    /**
     * false: count what a rewrite would do and emit the original's bytes unchanged, touching no
     * state. That is what `scan` passes, and it is also the tool's own test of this file — a
     * no-substitution rewrite must reproduce the changeset's id exactly.
     */
    bool substitute = true;
};

//! What one rewrite found and what it did. The conditions are the census; the actions are the work.
struct RewriteCounts {
    int64_t tables = 0;  //!< table headers in the stream
    int64_t changes = 0; //!< changes, every table
    int64_t elementInserts = 0, elementUpdates = 0, elementDeletes = 0;

    int64_t insertsWithoutGuid = 0; //!< an insert whose new FederationGuid is NULL
    //! A delete whose **old** FederationGuid is NULL — the value a transformer reads, and the
    //! reason this rewriter exists. An `advance` cannot change it; this is the count it cannot fix.
    int64_t deletesWithoutGuid = 0;
    int64_t updateOldsWithoutGuid = 0; //!< an update that changed the guid away from NULL
    int64_t updateNewsWithoutGuid = 0; //!< an update that set the guid to NULL

    int64_t fieldsWritten = 0; //!< guid fields this rewrite replaced
    //! A NULL this rewrite could not fill, because the store has no guid for that id. Never
    //! guessed and never hidden: it means the state does not cover the history being rewritten
    //! (a fork started mid-timeline, or a seed that was never GUIDated).
    int64_t unresolved = 0;
    int64_t guidsAssigned = 0;    //!< guids chosen here
    int64_t newIncarnations = 0;  //!< of those, for an id that had been given a guid before
    int64_t insertsWhileAlive = 0; //!< an insert of an id the store still believed alive
    //! Inserts and updates whose guid the original brought itself, offered to the store. Only ids
    //! the store already knows get an entry from this — an element whose insert carried a guid
    //! carries it in its delete's old values too, so it never needs one.
    int64_t guidsObserved = 0;
    //! A FederationGuid that is neither NULL nor a 16-byte blob. Zero, or the column index is wrong
    //! — which is the check that makes a positional guess safe.
    int64_t unexpectedGuidValues = 0;

    int64_t bytesIn = 0, bytesOut = 0; //!< decompressed change-stream bytes

    void Add(RewriteCounts const& o);
};

/**
 * Rewrite `in` into `out`, substituting the guid column wherever it is NULL. `in` is never written.
 *
 * The prefix — the JSON document carrying `ContainsSchemaChanges` and the DDL — is copied
 * **verbatim**, which `ChangesetFileWriter` cannot do: it rebuilds the JSON with
 * `BeJsDocument::Stringify` and forces `ContainsSchemaChanges` true whenever there is DDL, so a
 * schema changeset round-tripped through it comes out with a different id.  Carrying the original 
 * bytes is what makes a no-op rewrite reproduce **every** id.
 */
DbResult RewriteChangeset(std::string const& in, std::string const& out, ElementGuidStore& store,
                          RewriteOptions const& options, RewriteCounts& counts, std::string& error);

/**
 * The id the rewritten changeset would have, without writing or compressing anything.
 *
 * The id is SHA1 over the parent id, the prefix and every decompressed byte, so computing it over
 * the rewriter's output stream is both the cheap way to check a whole corpus and the only honest
 * way to ask "did this rewrite change anything it did not mean to": with `substitute = false` the
 * id must come back **identical** to the original's.
 */
DbResult RewrittenChangesetId(std::string const& in, std::string const& parentId, ElementGuidStore& store,
                              RewriteOptions const& options, RewriteCounts& counts, std::string& id,
                              std::string& error);

/**
 * GUIDate the seed: a guid onto every `bis_Element` row that has none, and **nothing else**.
 *
 * `LastMod` is deliberately not written, and the triggers are suspended so that nothing else
 * writes it either. These guids travel in no changeset — they are already in the seed every
 * consumer starts from — so the row keeps the `LastMod` the real history gave it, and every later
 * changeset's old values still match the row they arrive at. That is what makes a replay of the
 * rewritten history conflict-free: a seed that moved `LastMod` would make every later update of a
 * GUIDated row arrive stale.
 *
 * Every assignment goes into the store at index 0, so the deletes that come later can find it.
 */
DbResult SeedNullGuids(Db& db, ElementGuidStore& store, SeedCounts& counts, int64_t limit);

//! `FederationGuid`'s ordinal in a table, from the file's own schema. -1 if there is no such column.
int FindColumnOrdinal(Db& db, char const* table, char const* column);

} // namespace guidatron
