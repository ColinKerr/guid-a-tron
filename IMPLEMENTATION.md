# guid-a-tron — implementation

This is all AI Slop.

How the tool works, and why each piece is the way it is. [`README.md`](README.md) is how to use it.

Citations are to the C++ sources this tool has to **agree** with but does not link
(`imodel-native/iModelCore/…`, written here as BeSQLite/… and ECDb/… and DgnCore/…) and to the
shipped JavaScript it has to agree with (`@itwin/core-backend`). What it is built from instead is
section 12.

## Contents

1. [The shape of the problem](#1-the-shape-of-the-problem)
2. [SQLite's changeset format](#2-sqlites-changeset-format)
3. [The file container, and its prefix](#3-the-file-container-and-its-prefix)
4. [The changeset id, as a test](#4-the-changeset-id-as-a-test)
5. [The rewriter](#5-the-rewriter)
6. [The substitution policy](#6-the-substitution-policy)
7. [The guid store](#7-the-guid-store)
8. [The seed, and LastMod](#8-the-seed-and-lastmod)
9. [Triggers, and why nothing is applied](#9-triggers-and-why-nothing-is-applied)
10. [What was not usable, and why](#10-what-was-not-usable-and-why)
11. [Limits](#11-limits)
12. [What this is built from](#12-what-this-is-built-from)

## 1. The shape of the problem

A transformer resolves a **deleted** element by its `federationGuid`, read from the changeset rather
than from the source file, because by the time the deletion is processed the row is gone:

```js
// ChangesetScanner.js:120-131, then IModelTransformer.js:1549-1572 -> getIdFromFederationGuid
if (change.$meta.op === "Deleted")
  addDeletionRecord(batch, { federationGuid: change.FederationGuid, … })
```

`ChangesetReader.js:128-152` reads those values with `stage: "Old"`. So a usable test iModel needs
guids **in its changesets' old values**, and there are only two ways to get them:

| | apply and re-author | rewrite |
| --- | --- | --- |
| how | apply each original changeset to a briefcase, write guids onto whatever arrived without one, and author the original's rows plus those writes as one changeset | substitute the guid field in the original changeset's own bytes |
| needs | a briefcase, an apply, the DDL phases, trigger suspension, `ChangeGroup` | a decompressor and a recompressor |
| a deleted element's guid | **cannot reach it** — an appended row carries only new values | yes |
| fidelity | the original's rows, re-sorted by `ChangeGroup` | the original's bytes, with guid fields replaced |

This tool rewrites. The cost of that choice is that it has to write SQLite's changeset format
itself, since BeSQLite can read a changeset's values and copy a whole changeset but cannot write one
*value*. Sections 2–4 are how that risk is bounded.

## 2. SQLite's changeset format

Implemented in [`src/ChangesetRecords.h`](src/ChangesetRecords.h), from the specification in
SQLite's own source — `BeSQLite/SQLite/sqlite3.c:233912`, *RECORD FORMAT* and *CHANGESET FORMAT*:

```
table header:  0x54 'T' | varint nCol | nCol bytes of PK flags | table name, nul-terminated
change:        opcode (0x12 insert, 0x17 update, 0x09 delete) | indirect flag
               | old record (delete, update) | new record (insert, update)
record:        nCol fields, each  0x00 undefined | 0x01 int (8 bytes big-endian)
               | 0x02 real (8 bytes big-endian) | 0x03 text (varint length + bytes, no nul)
               | 0x04 blob (varint length + bytes) | 0x05 NULL
```

Varints are SQLite's `sqlite3PutVarint`: 7 bits per byte, most significant group first, the high bit
meaning "another byte follows", and a ninth byte carrying a full 8 bits. A patchset leads with
`0x50 'P'` instead and is **rejected** — its records are not the same shape.

Two properties of the format are what make the substitution sound, and both are the format's own
words rather than an observation about one corpus:

- *"In the changeset format, the records that are part of INSERT or DELETE changes never contain any
  undefined fields"* — so a delete always carries the deleted row's `FederationGuid`. That is the
  field `ChangesetScanner` reads, and the one an appended row cannot revise.
- In an update's old record, *"all fields associated with table columns that are not PRIMARY KEY
  columns and are not modified by the UPDATE change are set to 'undefined'"* — so an update that
  does not touch the guid says nothing about it, and nothing has to be written.

**NULL and undefined are different single bytes** (`0x05` and `0x00`); the parser keeps them
distinct and the tests assert it.

The one thing a changeset cannot tell you is **which column is which**: a table header carries the
column count and the primary key flags, not the names. The ordinal is therefore configuration —
`seed` reads `PRAGMA table_info(bis_Element)` from the file itself and records it in the store
(10 on every real file checked), `--guid-column` overrides it, and the unit tests deliberately put
`FederationGuid` at column 1 so a hard-wired 10 fails.

Everything in the parser is a view over caller-owned bytes: a `Field` points into the buffer it was
parsed from and is valid only while that buffer lives.

## 3. The file container, and its prefix

A `.cs` file is not a bare change stream. `BeSQLite/ChangesetFile.cpp:41-110`:

```
22-byte ChangesetLzmaHeader:  16 00 | "ChangeSetLzma" | 00 00 00 | 10 00 | 02 00
then an LZMA2 stream holding:
    4-byte big-endian prefix length
    the prefix bytes, INCLUDING their nul terminator   (Utf8String::SizeInBytes() == size() + 1)
    the change stream
```

The prefix is a JSON document carrying `ContainsSchemaChanges` and the DDL. It exists in some changesets
and `ChangesetFileWriter` **cannot carry one through**: its constructor rebuilds the JSON with `BeJsDocument::Stringify` and
forces `ContainsSchemaChanges: true` whenever there is DDL, so a schema changeset round-tripped
through it comes out with a different id. Its `_Append` is also private, so bytes can only enter it
via `ReadFrom(Changes::Reader&)`.

Hence `RawChangesetWriter` ([`src/ChangesetRewrite.h`](src/ChangesetRewrite.h)): the same 22-byte
header, the same LZMA2 stream, the original's prefix length and bytes verbatim, and a **public**
`_Append`. The 22 header bytes are asserted against the literal above, which was read out of real
files with `hexdump`.

## 4. The changeset id, as a test

A changeset's id is SHA1 over the parent id as 20 raw bytes, the prefix bytes, and every
**decompressed** byte of the change stream, in order (`DgnCore/ChangesetTxns.cpp:724-772`; ported in
[`src/ChangesetId.h`](src/ChangesetId.h)). It is therefore:

- blind to how the bytes were compressed, so recompression is free;
- sensitive to every byte of content **and to their order**;
- sensitive to the prefix, so a rebuilt prefix moves it.

That makes it the complete test of this tool's reader, writer and prefix handling: stream a
changeset through the rewriter with **nothing substituted** and the id must come back identical.
`guid-a-tron scan` does exactly that, and `RewrittenChangesetId` computes it without writing or
compressing anything, which is what makes a pass over a whole history affordable.

## 5. The rewriter

[`src/ChangesetRewrite.cpp`](src/ChangesetRewrite.cpp). The core is `RewritingReader`, a
`ChangeStreamReader` that consumes one decompressed change stream and produces another:

```
ChangesetFileReader                          the original, decompressed, prefix handled
        |
        v
RewritingReader                              parse one unit, decide, emit
        |
        +--> RawChangesetWriter::ReadFrom()  the fork's .cs        (rewrite)
        +--> ChangesetIdGenerator::ReadFrom() the id only          (scan)
```

Being a `ChangeStreamReader` is what lets the same code serve both: `ChangeStreamWriter::ReadFrom`
pulls from it, so the rewrite and the id computation are the same traversal with a different sink.
The two interfaces are two virtual functions in [`src/ChangesetFile.h`](src/ChangesetFile.h), named
after the BeSQLite pair they replace (`Changes::Reader` and `ChangeStream`) because the shape is the
one that fits the job.

It is a streaming state machine, not a buffer-the-file design: `Fill()` takes 64 KB at a time from
the source and compacts what is left, `NextUnit()` parses either a table header or one change,
`Plan()` decides what to do, and `Emit`/`EmitRewritten` copy bytes to the output. A field whose
bytes have not all arrived yet reports `Parse::NeedMore` rather than guessing, and the loop fills
and retries. Peak memory is a few hundred KB regardless of changeset size.

Three invariants, all load-bearing:

- **Nothing is grouped.** `ChangeGroup` re-sorts rows within a table, which measurably changes a
  changeset's id even when it adds nothing. The rewriter never groups, so the order of changes and
  the order of fields within a change are the original's.
- **Only the target table, only the target column.** Every other byte is copied through unexamined.
- **A guid field is NULL, absent (undefined), or a 16-byte blob.** Anything else **stops the
  rewrite** with an error rather than being overwritten, because at that point the column ordinal
  is in doubt and writing would corrupt the file.

## 6. The substitution policy

In one line: *every NULL `FederationGuid` in the target table, in an old record or a new one,
becomes the guid that element's current incarnation holds in the fork.* Per opcode:

| change | the guid field | what happens |
| --- | --- | --- |
| insert | a 16-byte blob | the original brought its own guid — nothing written, and the store **observes** it, so a later delete of that id is explicable |
| insert | NULL | `NewIncarnation`: a guid is chosen, any guid that id held before is retired, and the new value is written into the record |
| delete | a 16-byte blob | nothing written; the store marks the id dead |
| delete | NULL | `Lookup` the id's current guid and write it into the **old** record — the case the whole tool exists for. Not found: `unresolved` is counted, nothing is written, nothing is invented. Then the id is marked dead |
| update | both undefined | the common case: the update does not mention the guid, so neither does the rewrite |
| update | old NULL | the original is filling in a guid itself; the fork's existing guid is written into the old record so the change still matches the row it will land on |
| update | new NULL | the original is *clearing* the guid; the fork keeps one (the store's, else the old record's, else a fresh incarnation), because an element without a guid is the thing being removed |
| update | new blob | the original's own value stands, and the store observes it |

`scan` runs the same `Plan()` with `substitute = false`: every counter is computed, no bytes change,
no state is written. That is why the census and the format check are one pass, and why the census
numbers come from exactly the code that would do the work.

## 7. The guid store

`guids.db`, beside the seed. A guid derived from (scope, element id) could be recomputed from a
delete's primary key alone, with no state at all. Two things break that:

- **Random guids are not a function of anything.** Random is the default and the production-faithful
  choice: a connector generates a guid when it creates an element. The guid an insert was given has
  to be remembered for the delete that may follow tens of thousands of changesets later.
- **Element ids are reused.** The platform should prevent this (element ids come from
  `BeBriefcaseBasedIdSequence`, a monotonic high-water mark), but it has happened. Two incarnations
  of one id are two different elements and must not share a guid.

```sql
CREATE TABLE guid_assignment(          -- the living incarnation of every id we gave a guid to
  ElementId INTEGER PRIMARY KEY, Guid BLOB NOT NULL, Incarnation INTEGER NOT NULL,
  Alive INTEGER NOT NULL, Assigned INTEGER NOT NULL, SinceIndex INTEGER NOT NULL);
CREATE TABLE guid_retired(             -- what each previous incarnation held
  ElementId INTEGER NOT NULL, Incarnation INTEGER NOT NULL, Guid BLOB NOT NULL,
  SinceIndex INTEGER NOT NULL, RetiredIndex INTEGER NOT NULL,
  PRIMARY KEY(ElementId, Incarnation));
CREATE TABLE store_meta(Name TEXT PRIMARY KEY, Value TEXT NOT NULL);
```

`Assigned = 0` marks a guid that is the original's own, read out of a changeset rather than chosen
here. `store_meta` holds `guidPolicy`, `derivationScope`, `guidColumn`, `seed`, `baseline`,
`forkPointChangesetId`, `forkPointIndex`, `lastIndex`, `lastChangesetId`, `tool`.

Operationally:

- Loaded into memory for the run (`unordered_map`, ~40 bytes an entry) and flushed in one
  transaction after each changeset, so a long run is resumable and a crash costs one changeset.
- **Only elements this tool gave a guid to need an entry**: an element whose insert already
  carried a guid carries it in its delete's old values too, so there is nothing to look up. Entries that do exist are still updated from the original's own bytes, so `alive` stays
  honest and a reuse is detectable.
- On reopen the **store's** policy wins, not the caller's; a run that mixed random and derived guids
  would be neither reproducible nor explicable. If `--guids` was passed explicitly and disagrees,
  the open fails rather than quietly doing something else.
- `rewrite` refuses an index at or behind the store's position unless `--force`: the state is a
  forward-only fold over the history, and out of order it is wrong rather than merely stale.

With `--guids derived` the incarnation is part of the derivation — SHA1 over (scope, element id,
incarnation), UUID version nibble 5 and the RFC 4122 variant bits — so a reused id still gets two
different guids. The incarnation is left out of the hashed string when it is 1.

## 8. The seed, and LastMod

`SeedNullGuids` writes `FederationGuid` on every `bis_Element` row that has none, and **nothing
else**. It does not write `LastMod`, and it suspends the triggers so that nothing else does either.

That is only possible because these guids travel in **no changeset**: the seed is the file every
consumer starts from. The constraint and the finding behind it:

```sql
CREATE TRIGGER bis_Element_CurrentTimeStamp AFTER UPDATE ON bis_Element
WHEN old.LastMod=new.LastMod AND old.LastMod!=julianday('now')
BEGIN UPDATE bis_Element SET LastMod=julianday('now') WHERE Id=new.Id; END
```
(`ECDb/ECDb/IntegrityChecker.cpp:350`)

The trigger fires precisely when an update leaves `LastMod` at the value it already held — and it
does not care whether the column was in the `SET` clause, so a guid-only write cannot keep a row's
`LastMod` by restoring it afterwards. Worse, **it fires again when a changeset is applied**, on a
consumer's connection, where nobody puts the value back: a guid-only changeset measurably replays
with every row at `julianday('now')`. Both halves are asserted in the unit tests.

So guids that must survive a replay have to travel with an explicit `LastMod` — and guids that
travel in no changeset at all do not have to survive anything. The second is strictly better, and it
is what makes a rewritten fork **conflict-free on replay**: the seed differs from the baseline in
exactly one column, so every later changeset's old values still match the rows they land on. The
check that this holds is run, not assumed: `SeedNullGuids` re-reads each row's `LastMod` after
writing and reports `lastModUnstable`, and `SUM(LastMod)` over the seed matches the baseline's to
the last decimal.

The alternative of dropping the trigger is worse than it looks: its exact SQL is part of what
`IntegrityChecker::CheckEcProfile` requires of a valid file, and
`DerivedDbStructures::AddCurrentTimeStampTriggers` regenerates it from the column constraints on the
next schema import, so a drop is not even durable.

## 9. Triggers, and why nothing is applied

A `.bim` carries its spatial-index triggers in the file, and their bodies call DgnPlatform's own SQL
functions (`ECDb/ECDb/IntegrityChecker.cpp:354`, built by `DgnCore/DgnDbSchema.cpp:186-204`):

```sql
CREATE TRIGGER dgn_rtree_ins AFTER INSERT ON bis_GeometricElement3d … SELECT new.ElementId,
  DGN_bbox_value(bb,0), … FROM (SELECT DGN_placement_aabb(DGN_placement(DGN_point(…),
  DGN_angles(…), DGN_bbox(…)))) as bb
```

This tool links neither DgnPlatform nor ECDb, so `DGN_bbox` does not exist on its connection and any
statement that fires such a trigger would fail. Those triggers fire on insert and delete; the seed only ever updates
one column of existing rows, so it never reaches them. What the seed does need the suspension for is
`bis_Element_CurrentTimeStamp`, for the reason in section 8.

That is also why nothing here applies a changeset. The index those triggers maintain is derived
data: `TxnManager::_FilterTable` (`DgnCore/TxnManager.cpp:1024-1033`) keeps `dgn_SpatialIndex` and
friends out of changesets as *"redundant data that will be automatically updated when the changeset
is applied"*, and a consumer applies with DgnDb, triggers live, rebuilding the index exactly as it
does for the original changesets. A rewrite copies rows rather than adding any, so the fork's
changesets carry exactly what the originals carried.

## 10. What was not usable, and why

- **`ChangesetFileWriter`** rebuilds the prefix and forces `ContainsSchemaChanges` when there is
  DDL, and its `_Append` is private. Fine for a changeset with no prefix; wrong for those that
  have one. Replaced by `RawChangesetWriter` (section 3).
- **`ChangeGroup`** re-sorts rows within a table. It is the right tool for combining changesets and
  the wrong one for preserving a changeset: it changes the id while adding nothing.

## 11. Limits

- **One column of one table.** `bis_Element.FederationGuid`. Guids on other classes, aspects or
  relationships are out of scope, as is any other column.
- **Forward only.** The guid state is a fold over the history in index order. A fork begun partway
  through a history can only resolve deletions of elements it has seen or that the seed covered;
  anything else is reported as `unresolved`.
- **Patchsets are rejected**, not rewritten.
- **The column ordinal is configuration.** It is read from the seed's own schema and recorded, but a
  changeset from a file with a different `bis_Element` layout would need `--guid-column`.
- **Rewriting is bounded by LZMA recompression**, at ~2.8 MB/s of compressed input.
- **The briefcase bookkeeping is unverified against a hub.** The seed keeps the baseline's
  `ParentChangeSetId`/`parentChangeset`. Whether iModelHub would accept the seed and the fork's
  changesets as a new iModel is untested.
- **No transform has been run across the fork.** The deletions carry their guids, but propagating a
  deletion through `federationGuid` in an actual incremental transform is untested.

## 12. What this is built from

Nothing of iTwin's is linked. SQLite comes from the public amalgamation, the LZMA SDK is vendored,
JSON is one header, and what BeSQLite would otherwise provide — `Db`, `Statement`, `BeGuid`, `SHA1`,
the changeset container — is in `src/`.

| | |
| --- | --- |
| SQLite | 3.53.4, `sqlite-amalgamation-3530400.zip` from sqlite.org, pinned by SHA-256 in `cmake/dependencies.cmake` |
| LZMA | the SDK iTwin ships (`libsrc/compress/lzma`, 7 files, `_7ZIP_ST`), vendored in `third_party/lzma` |
| JSON | `nlohmann/json` 3.11.3, one header, pinned by SHA-256 |

### The flags, and why each one

The list is in `CMakeLists.txt` beside the target, as `PUBLIC` definitions — several of them decide
what `sqlite3.h` *declares*, so a consumer compiled without them would see a different SQLite from
the one it links. It is iTwin's own list
(`imodel-native/iModelCore/BeSQLite/SQLite/bentley-sqlite.c`) minus what this tool has no use for:

- **Mandatory to open a `.bim` at all.** `SQLITE_ENABLE_RTREE` and `SQLITE_ENABLE_FTS5`: every file
  checked carries `dgn_SpatialIndex USING rtree(...)` and `dgn_fts_idx using fts5(...)`, and a
  module that is not compiled in makes its table unusable. `FTS4` and `GEOPOLY` follow iTwin's build
  so that a file written by it cannot surprise this one.
- **Limits.** `SQLITE_MAX_COLUMN=2200` is taken from ECDb as are `MAX_LENGTH`, `MAX_VARIABLE_NUMBER`, `MAX_EXPR_DEPTH`.
- **The session extension** (`SQLITE_ENABLE_SESSION`, `SQLITE_ENABLE_PREUPDATE_HOOK`), which only
  the tests use — see below. The tool's own commands parse and emit changeset bytes directly and
  never call it.
- **Fidelity with the build that wrote these files**, so that what this tool writes is encoded and
  enforced the same way: `ENABLE_NULL_TRIM` (which changes the on-disk record encoding — trailing
  NULL columns are omitted), `DEFAULT_FOREIGN_KEYS`, `DEFAULT_CACHE_SIZE`, `USE_URI`,
  `ENABLE_MATH_FUNCTIONS`, `ENABLE_COLUMN_METADATA`.
- **Deliberately not set**: `ENABLE_ZIPVFS` (mentioned in the imodel-native code but not used),
  `OMIT_AUTOINIT`, `SQLITE_DEBUG`, the introspection vtabs, and the DLL-export plumbing. Nor is
  `shathree.c` or `blockcachevfs.c` compiled.

### ICU is not needed

The only ICU in BeSQLite is the `NOCASE` collation. `DbFile::SetNoCaseCollation`
(`BeSQLite.cpp:1529-1562`) installs either `nocaseCollatingFuncASCII` — `sqlite3_strnicmp` plus a
length tiebreak, which is byte-for-byte what SQLite's built-in `NOCASE` does — or an ICU
`ucol_open("latin1")` collator at primary strength. **The default is `NoCaseCollation::ASCII`**
(`BeSQLite.cpp:1478`) and nothing in `imodel-native` or `iModelJsNode` ever calls that setter; only
a unit test does. So every `NOCASE` index in every `.bim` was ordered by the comparator this build
already has.

Schema objects in an iModel do use `COLLATE NOCASE`, `be_Local.Name` and `bis_Element.CodeValue`
among them. Two precautions, neither of which costs anything:

- The tool writes only `bis_Element.FederationGuid`, whose index
  (`ix_bis_Element_FederationGuid`, unique, partial) is on a BLOB with no collation.
- `Db::QueryBriefcaseLocalValue` and `Db::GetDbGuid` force `COLLATE BINARY`, so they scan a
  six-row table instead of seeking an index whose ordering came from somewhere else.

### LZMA

Why the SDK is vendored rather than downloaded is in
[`third_party/lzma/README.md`](third_party/lzma/README.md).

The framing reproduced in `src/Lzma.cpp` is `BeLzma.cpp`'s: one properties byte from
`Lzma2Enc_WriteProperties`, then LZMA2 chunks, then a single `0x00`. And the page discipline is
`LzmaEncoder::CompressNextPage`'s — a whole `Lzma2Enc_Encode` per page with the `0x00` it appends
dropped, so the file is a concatenation of per-page chunk sequences with one terminator at the end.
That costs some compression, since each page resets the dictionary, but it is what the existing
changesets are. Parameters are `ChangesetFileWriter`'s defaults: 16 MB dictionary, level 7.

### What the tests use the session extension for

The tool authors no changesets, so nothing in `src/` records changes. The tests need changesets to
rewrite, and they come from `sqlite3session_create`/`_attach`/`_changeset` with the same table
filter iTwin uses (`TxnManager::_FilterTable`), while the assertions about what a rewritten
changeset *says* are made with `sqlite3changeset_start`/`_old`/`_new`. Both are the reference
implementation of the format, so a rewrite that agrees with them is not merely self-consistent.
