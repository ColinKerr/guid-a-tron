/*---------------------------------------------------------------------------------------------
 * guid-a-tron — CLI.
 *
 *   guid-a-tron seed     --baseline <file> --out <dir>      the GUIDated seed and its guid store
 *   guid-a-tron rewrite  --fork <dir> --changeset-list <f>  the rewritten changesets
 *   guid-a-tron scan     --changeset-list <file>            the census, and the round-trip check
 *   guid-a-tron coverage --imodel <file>                    how many elements have a guid
 *   guid-a-tron id       --changeset <file>                 a changeset's id, given its parent
 *
 * `seed` copies the baseline — the input is opened read-only and never written — and gives every
 * element that has no `FederationGuid` one, touching no other column. `rewrite` then edits the
 * same column inside the original changesets' own bytes, so that an element's guid is in the
 * history and not merely in the file: a deleted element's guid lives in a change's **old** values,
 * which only a rewrite can reach. `scan` is the census and the format check.
 *
 *--------------------------------------------------------------------------------------------*/
#include "ChangesetFile.h"
#include "ChangesetId.h"
#include "ChangesetRewrite.h"
#include "Files.h"
#include "GuidATron.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <stdio.h>
#include <string>
#include <time.h>
#include <vector>

using namespace guidatron;
using Json = nlohmann::json;

namespace {

char const* USAGE =
    "guid-a-tron — give a real iModel's history federationGuids, changing nothing else\n"
    "\n"
    "  guid-a-tron seed     --baseline <file> --out <dir> [--guids random|derived] [--limit <n>]\n"
    "  guid-a-tron rewrite  --fork <dir> --changeset <file> --index <n> [options]\n"
    "  guid-a-tron rewrite  --fork <dir> --changeset-list <file> [options]\n"
    "  guid-a-tron scan     --changeset <file> | --changeset-list <file> [--guid-column <n>]\n"
    "  guid-a-tron coverage --imodel <file>\n"
    "  guid-a-tron id       --changeset <file> [--parent <changesetId>]\n"
    "\n"
    "seed options\n"
    "  --guids random|derived  random (default) is what the platform does for an element inserted\n"
    "                          without a guid; derived is reproducible across rebuilds, which\n"
    "                          production does not do — the store records which was used.\n"
    "  --limit <n>             GUIDate at most n elements; 0 GUIDates none, which is how to\n"
    "                          measure what the seed itself costs.\n"
    "\n"
    "rewrite/scan options\n"
    "  --changeset-list <file>  one `<index> <path>` per line, in ascending index order; `#`\n"
    "                          comments and blank lines are ignored.\n"
    "  --guid-column <n>       bis_Element.FederationGuid's ordinal, if it is not what the seed\n"
    "                          recorded. A changeset names columns by position only.\n"
    "  --description <text>    the description recorded for each rewritten changeset.\n"
    "  --force                 rewrite a changeset at or behind the store's position. The guid\n"
    "                          state is a forward-only fold over the history; out of order it is\n"
    "                          wrong, not merely stale.\n"
    "\n"
    "The baseline is copied and never modified. A fork lives in <dir>: seed.bim, guids.db,\n"
    "changesets/NNNNN-<id>.cs, and manifest.json. There is no working copy, because nothing here\n"
    "applies changesets.\n";

struct Args {
    std::string command;
    std::string baseline, out, fork, changeset, changesetList, imodel, parent, description;
    GuidPolicy guids = GuidPolicy::Random;
    bool guidsExplicit = false; //!< so `rewrite` can adopt the policy the seed's store recorded
    int64_t limit = -1;
    int64_t index = -1;
    int guidColumn = -1; //!< -1: whatever the seed recorded in the store
    bool force = false;
    bool ok = true;
    std::string error;
};

Args Parse(int argc, char** argv) {
    Args a;
    if (argc < 2) {
        a.ok = false;
        return a;
    }
    a.command = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string flag = argv[i];
        auto next = [&](std::string& into) {
            if (i + 1 >= argc) {
                a.ok = false;
                a.error = flag + " needs a value";
                return;
            }
            into = argv[++i];
        };
        if (flag == "--baseline") next(a.baseline);
        else if (flag == "--out") next(a.out);
        else if (flag == "--fork") next(a.fork);
        else if (flag == "--changeset") next(a.changeset);
        else if (flag == "--changeset-list") next(a.changesetList);
        else if (flag == "--force") a.force = true;
        else if (flag == "--imodel") next(a.imodel);
        else if (flag == "--parent") next(a.parent);
        else if (flag == "--description") next(a.description);
        else if (flag == "--guids") {
            std::string v;
            next(v);
            a.guidsExplicit = true;
            if (v == "derived") a.guids = GuidPolicy::Derived;
            else if (v == "random") a.guids = GuidPolicy::Random;
            else { a.ok = false; a.error = "--guids must be random or derived"; }
        } else if (flag == "--limit") {
            std::string v; next(v); a.limit = atoll(v.c_str());
        } else if (flag == "--guid-column") {
            std::string v; next(v); a.guidColumn = atoi(v.c_str());
        } else if (flag == "--index") {
            std::string v; next(v); a.index = atoll(v.c_str());
        } else {
            a.ok = false;
            a.error = "unknown option " + flag;
        }
    }
    return a;
}

//! The reason a command gives, preferring the explanation over SQLite's text for a code that only
//! means "this tool said no". SQLite's text is kept when it is the only thing there is.
char const* Why(DbResult rc, std::string const& error) {
    return error.empty() ? DbResultText(rc) : error.c_str();
}

int Fail(char const* what, DbResult rc) {
    fprintf(stderr, "guid-a-tron: %s: %s (%d)\n", what, DbResultText(rc), (int)rc);
    return 1;
}

bool OpenReadWrite(Db& db, std::string const& path) {
    DbResult rc = db.OpenDb(path, Db::OpenMode::ReadWrite);
    if (DB_OK != rc) {
        fprintf(stderr, "guid-a-tron: cannot open %s read-write: %s\n", path.c_str(), DbResultText(rc));
        return false;
    }
    return true;
}

void PrintCoverage(char const* label, Coverage const& c) {
    printf("%s: %lld elements, %lld without a guid (%.3f%% covered), %lld distinct guids\n", label,
           (long long)c.elements, (long long)c.withoutGuid,
           c.elements == 0 ? 0.0 : 100.0 * (double)(c.elements - c.withoutGuid) / (double)c.elements,
           (long long)c.distinctGuids);
}

std::string ChangesetDir(std::string const& forkDir) { return forkDir + "/changesets"; }

/**
 * The manifest, which a resumed `rewrite` has to extend rather than replace.
 *
 * An array of {id, index, parentId, description, changesType, pathname},
 * plus a `fork` block recording what produced the artifact, and a per-changeset record of what the
 * rewrite did to it: how many guid fields it wrote, how many it could not resolve, and which
 * original file it came from.
 */
std::string ManifestPath(std::string const& forkDir) { return forkDir + "/manifest.json"; }

Json LoadManifest(std::string const& forkDir) {
    FILE* f = fopen(ManifestPath(forkDir).c_str(), "rb");
    if (f == nullptr)
        return Json::object();
    std::string text;
    char buffer[4096];
    size_t read = 0;
    while ((read = fread(buffer, 1, sizeof(buffer), f)) > 0)
        text.append(buffer, read);
    fclose(f);
    Json manifest = Json::parse(text, nullptr, false);
    if (manifest.is_discarded() || !manifest.is_object()) {
        fprintf(stderr, "guid-a-tron: %s is not readable JSON; it will be replaced\n",
                ManifestPath(forkDir).c_str());
        return Json::object();
    }
    return manifest;
}

bool SaveManifest(std::string const& forkDir, Json const& manifest) {
    std::string path = ManifestPath(forkDir);
    FILE* f = fopen(path.c_str(), "w");
    if (f == nullptr) {
        fprintf(stderr, "guid-a-tron: cannot write %s\n", path.c_str());
        return false;
    }
    std::string text = manifest.dump(2) + "\n";
    fwrite(text.c_str(), 1, text.size(), f);
    fclose(f);
    printf("wrote %s\n", path.c_str());
    return true;
}

int CommandId(Args const& a) {
    if (a.changeset.empty()) {
        fprintf(stderr, "guid-a-tron id needs --changeset\n");
        return 2;
    }
    std::string id = ComputeChangesetId(a.parent, a.changeset);
    if (id.empty()) {
        fprintf(stderr, "guid-a-tron: could not read %s\n", a.changeset.c_str());
        return 1;
    }
    printf("%s\n", id.c_str());
    return 0;
}

//=============================================================================================
// The seed, and the rewrite. See ChangesetRewrite.h.
//=============================================================================================

std::string SeedDb(std::string const& forkDir) { return forkDir + "/seed.bim"; }
std::string StorePath(std::string const& forkDir) { return forkDir + "/guids.db"; }

//! `NNNNN-<id>.cs`, index-prefixed.
std::string RewrittenName(int64_t index, std::string const& id) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%05lld-", (long long)index);
    return std::string(buf) + id.c_str() + ".cs";
}

//! `<index> <path>` per line; `#` comments and blank lines ignored. The path may contain spaces.
bool LoadChangesetList(std::string const& path, std::vector<std::pair<int64_t, std::string>>& out) {
    FILE* f = fopen(path.c_str(), "r");
    if (f == nullptr) {
        fprintf(stderr, "guid-a-tron: cannot read %s\n", path.c_str());
        return false;
    }
    char line[4096];
    int lineNumber = 0;
    while (fgets(line, sizeof(line), f) != nullptr) {
        ++lineNumber;
        char* at = line;
        while (*at == ' ' || *at == '\t')
            ++at;
        if (*at == '#' || *at == '\n' || *at == '\r' || *at == 0)
            continue;
        char* end = nullptr;
        long long index = strtoll(at, &end, 10);
        if (end == at) {
            fprintf(stderr, "guid-a-tron: %s:%d does not start with a changeset index\n", path.c_str(),
                    lineNumber);
            fclose(f);
            return false;
        }
        while (*end == ' ' || *end == '\t')
            ++end;
        std::string file(end);
        while (!file.empty() && (file.back() == '\n' || file.back() == '\r' || file.back() == ' '))
            file.pop_back();
        if (file.empty()) {
            fprintf(stderr, "guid-a-tron: %s:%d has an index but no path\n", path.c_str(), lineNumber);
            fclose(f);
            return false;
        }
        out.push_back({(int64_t)index, file});
    }
    fclose(f);
    return true;
}

void PrintRewriteCounts(char const* label, RewriteCounts const& c) {
    printf("%s %lld change(s) in %lld table(s); bis_Element: %lld insert(s), %lld update(s), "
           "%lld delete(s)\n", label, (long long)c.changes, (long long)c.tables,
           (long long)c.elementInserts, (long long)c.elementUpdates, (long long)c.elementDeletes);
    printf("%s   without a guid: %lld insert(s), %lld delete old value(s), %lld update old, "
           "%lld update new\n", label, (long long)c.insertsWithoutGuid,
           (long long)c.deletesWithoutGuid, (long long)c.updateOldsWithoutGuid,
           (long long)c.updateNewsWithoutGuid);
    printf("%s   written: %lld field(s), %lld guid(s) assigned (%lld new incarnation(s)), "
           "%lld observed, %lld unresolved\n", label, (long long)c.fieldsWritten,
           (long long)c.guidsAssigned, (long long)c.newIncarnations, (long long)c.guidsObserved,
           (long long)c.unresolved);
    if (c.unexpectedGuidValues > 0 || c.insertsWhileAlive > 0)
        printf("%s   WARNING: %lld guid value(s) that are not guids, %lld insert(s) of a live id\n",
               label, (long long)c.unexpectedGuidValues, (long long)c.insertsWhileAlive);
}

// Counts are written as JSON integers, which nlohmann holds exactly at this width.
Json RewriteCountsJson(RewriteCounts const& c) {
    Json j = Json::object();
    j["changes"] = c.changes;
    j["elementInserts"] = c.elementInserts;
    j["elementUpdates"] = c.elementUpdates;
    j["elementDeletes"] = c.elementDeletes;
    j["insertsWithoutGuid"] = c.insertsWithoutGuid;
    j["deletesWithoutGuid"] = c.deletesWithoutGuid;
    j["updateOldsWithoutGuid"] = c.updateOldsWithoutGuid;
    j["updateNewsWithoutGuid"] = c.updateNewsWithoutGuid;
    j["guidFieldsWritten"] = c.fieldsWritten;
    j["guidsAssigned"] = c.guidsAssigned;
    j["newIncarnations"] = c.newIncarnations;
    j["guidsObserved"] = c.guidsObserved;
    j["unresolved"] = c.unresolved;
    j["bytesIn"] = c.bytesIn;
    j["bytesOut"] = c.bytesOut;
    return j;
}

/**
 * `seed` — the baseline with its guid-less elements filled in, and nothing else touched.
 *
 * It authors **no changeset**: the seed is the artifact a consumer starts from, and the fork's
 * history is the rewritten originals. That is also why it does not write `LastMod` — see
 * `SeedNullGuids`.
 */
int CommandSeed(Args const& a) {
    if (a.baseline.empty() || a.out.empty()) {
        fprintf(stderr, "guid-a-tron seed needs --baseline and --out\n");
        return 2;
    }
    if (!CreateDirectories(a.out) || !CreateDirectories(ChangesetDir(a.out))) {
        fprintf(stderr, "guid-a-tron: cannot create %s\n", a.out.c_str());
        return 1;
    }

    std::string error;
    std::string to = SeedDb(a.out);
    if (!CopyFileFailIfPresent(a.baseline, to, error)) {
        fprintf(stderr, "guid-a-tron: %s\n", error.c_str());
        return 1;
    }
    printf("copied %s -> %s\n", a.baseline.c_str(), to.c_str());

    Db db;
    if (!OpenReadWrite(db, SeedDb(a.out)))
        return 1;

    // The guid column's ordinal comes from the seed's own schema. A changeset names columns by
    // position only, so this is the one number the rewriter cannot read out of a changeset.
    int guidColumn = FindColumnOrdinal(db, "bis_Element", "FederationGuid");
    if (guidColumn < 0) {
        fprintf(stderr, "guid-a-tron: %s has no bis_Element.FederationGuid column\n",
                SeedDb(a.out).c_str());
        return 1;
    }
    printf("bis_Element.FederationGuid is column %d\n", guidColumn);

    // Where the baseline sits in the original timeline: the fork's first rewritten changeset
    // follows this changeset, and its parent id is this id.
    std::string forkPointId, parentChangesetJson;
    db.QueryBriefcaseLocalValue(forkPointId, "ParentChangeSetId");
    db.QueryBriefcaseLocalValue(parentChangesetJson, "parentChangeset");
    int64_t forkPointIndex = 0;
    if (!parentChangesetJson.empty()) {
        Json parsed = Json::parse(parentChangesetJson, nullptr, false);
        if (!parsed.is_discarded() && parsed.is_object())
            forkPointIndex = parsed.value("index", (int64_t)0);
    }
    printf("fork point: changeset %s at index %lld\n",
           forkPointId.empty() ? "(none)" : forkPointId.c_str(), (long long)forkPointIndex);

    Coverage before = MeasureCoverage(db);
    PrintCoverage("before", before);

    ElementGuidStore store;
    Guid scope = db.GetDbGuid();
    DbResult rc = store.Open(StorePath(a.out), a.guids, scope, true, error);
    if (DB_OK != rc) {
        fprintf(stderr, "guid-a-tron: %s\n", error.c_str());
        return Fail("opening the guid store", rc);
    }

    SeedCounts counts;
    rc = SeedNullGuids(db, store, counts, a.limit);
    if (DB_OK != rc)
        return Fail("GUIDating the seed", rc);
    rc = db.SaveChanges("guid-a-tron: seed guids");
    if (DB_OK != rc)
        return Fail("saving the seed", rc);
    printf("GUIDated %lld element(s); LastMod untouched on %s\n", (long long)counts.elementsGuidated,
           counts.lastModUnstable == 0 ? "all of them" : "some of them — WARNING");

    char buf[64];
    snprintf(buf, sizeof(buf), "%d", guidColumn);
    store.SetMeta("guidColumn", buf);
    store.SetMeta("seed", SeedDb(a.out).c_str());
    store.SetMeta("baseline", a.baseline.c_str());
    store.SetMeta("forkPointChangesetId", forkPointId);
    snprintf(buf, sizeof(buf), "%lld", (long long)forkPointIndex);
    store.SetMeta("forkPointIndex", buf);
    rc = store.Flush();
    if (DB_OK != rc)
        return Fail("writing the guid store", rc);
    printf("guid store: %s (%lld assignment(s))\n", StorePath(a.out).c_str(),
           (long long)store.Size());

    Coverage after = MeasureCoverage(db);
    PrintCoverage("after ", after);
    db.CloseDb();
    store.Close();

    Json manifest = Json::object();
    Json fork = Json::object();
    fork["tool"] = "guid-a-tron";
    fork["mode"] = "rewrite";
    fork["baseline"] = a.baseline;
    fork["seed"] = SeedDb(a.out);
    fork["guidStore"] = StorePath(a.out);
    fork["guidPolicy"] = a.guids == GuidPolicy::Derived ? "derived" : "random";
    fork["derivationScope"] = scope.ToString();
    fork["guidColumn"] = guidColumn;
    fork["forkPointChangesetId"] = forkPointId;
    fork["forkPointIndex"] = forkPointIndex;
    fork["elementsBefore"] = before.elements;
    fork["withoutGuidBefore"] = before.withoutGuid;
    fork["withoutGuidAfter"] = after.withoutGuid;
    fork["elementsGuidated"] = counts.elementsGuidated;
    fork["note"] =
        "The seed is the baseline with a federationGuid on every element that had none and "
        "LastMod left exactly as the real history left it: these guids travel in no changeset, so "
        "nothing has to be replay-stable and no later original update can arrive with a stale "
        "LastMod. The fork's changesets are the originals with bis_Element.FederationGuid "
        "substituted wherever it was NULL — including the old values of deletes, which an apply-"
        "and-reauthor pass cannot reach. Guids are chosen once per element incarnation and kept "
        "in guids.db, because a history can delete an element id and re-insert it later.";
    manifest["fork"] = fork;
    manifest["changesets"] = Json::array();
    SaveManifest(a.out, manifest);
    printf("\nseed: %s\n", SeedDb(a.out).c_str());
    return after.withoutGuid == 0 || a.limit >= 0 ? 0 : 3;
}

/**
 * `rewrite` — one original changeset, or a list of them, as the fork's own.
 *
 * Every byte that is not a NULL `bis_Element.FederationGuid` is the original's, in the original's
 * order. Nothing is grouped: `ChangeGroup` re-sorts rows within a table, which measurably changes
 * a changeset's id even when it adds nothing, so the rewriter touches neither the order of changes
 * nor the order of fields within one.
 */
int CommandRewrite(Args const& a) {
    if (a.fork.empty()) {
        fprintf(stderr, "guid-a-tron rewrite needs --fork\n");
        return 2;
    }
    std::vector<std::pair<int64_t, std::string>> work;
    if (!a.changesetList.empty()) {
        if (!LoadChangesetList(a.changesetList, work))
            return 1;
    } else if (!a.changeset.empty()) {
        if (a.index <= 0) {
            fprintf(stderr, "guid-a-tron rewrite needs --index with --changeset\n");
            return 2;
        }
        work.push_back({a.index, a.changeset});
    } else {
        fprintf(stderr, "guid-a-tron rewrite needs --changeset or --changeset-list\n");
        return 2;
    }

    ElementGuidStore store;
    std::string error;
    // The scope is the store's, recorded when the seed created it; the one passed here only has to
    // survive the policy check.
    DbResult rc = store.Open(StorePath(a.fork), a.guids, Guid(), false, error, a.guidsExplicit);
    if (DB_OK != rc) {
        fprintf(stderr, "guid-a-tron: %s\n", error.c_str());
        return 1;
    }
    printf("guid store: %lld assignment(s), %lld retired, policy %s\n", (long long)store.Size(),
           (long long)store.Retired(), store.Policy() == GuidPolicy::Derived ? "derived" : "random");

    RewriteOptions options;
    std::string stored;
    if (a.guidColumn >= 0)
        options.guidColumn = a.guidColumn;
    else if (store.GetMeta("guidColumn", stored) && !stored.empty())
        options.guidColumn = atoi(stored.c_str());
    printf("rewriting bis_Element column %d\n", options.guidColumn);

    // The chain: the first rewritten changeset follows the baseline's own parent, and every one
    // after follows the one this tool wrote. Kept in the store so a batch can be resumed.
    std::string parentId = store.LastChangesetId();
    int64_t lastIndex = store.LastIndex();
    if (parentId.empty()) {
        store.GetMeta("forkPointChangesetId", parentId);
        std::string forkPointIndex;
        if (store.GetMeta("forkPointIndex", forkPointIndex))
            lastIndex = atoll(forkPointIndex.c_str());
    }

    Json manifest = LoadManifest(a.fork);
    if (!manifest.contains("changesets") || !manifest["changesets"].is_array())
        manifest["changesets"] = Json::array();

    RewriteCounts total;
    int64_t done = 0;
    bool failed = false;
    for (auto const& item : work) {
        int64_t index = item.first;
        // Order is not a preference: the store is a forward-only fold over the history, so a
        // changeset applied out of order would be given a guid from a state that never existed.
        if (index <= lastIndex && !a.force) {
            fprintf(stderr, "guid-a-tron: changeset %lld is at or behind the store's position "
                            "(%lld) — rewriting out of order would corrupt the guid state; pass "
                            "--force only if you know why\n",
                    (long long)index, (long long)lastIndex);
            failed = true;
            break;
        }
        std::string const& original = item.second;
        if (!FileExists(original)) {
            fprintf(stderr, "guid-a-tron: no such changeset: %s\n", item.second.c_str());
            failed = true;
            break;
        }
        std::string pending = ChangesetDir(a.fork) + "/pending.cs";
        RemoveFileIfPresent(pending);
        options.index = index;
        RewriteCounts counts;
        rc = RewriteChangeset(original, pending, store, options, counts, error);
        if (DB_OK != rc) {
            fprintf(stderr, "guid-a-tron: rewriting %s failed: %s\n", item.second.c_str(),
                    Why(rc, error));
            failed = true;
            break;
        }
        std::string id = ComputeChangesetId(parentId, pending);
        if (id.empty()) {
            fprintf(stderr, "guid-a-tron: the rewritten changeset at %lld could not be read back\n",
                    (long long)index);
            failed = true;
            break;
        }
        std::string finalPath = ChangesetDir(a.fork) + "/" + RewrittenName(index, id);
        RemoveFileIfPresent(finalPath);
        if (!MoveFileOver(pending, finalPath)) {
            fprintf(stderr, "guid-a-tron: cannot move %s into place\n", pending.c_str());
            failed = true;
            break;
        }

        uint64_t bytes = 0;
        FileSize(finalPath, bytes);
        bool containsSchemaChanges = false;
        {
            // Read back what was just written, which also proves the file opens.
            ChangesetFileReader written;
            std::string readError;
            if (DB_OK == written.Open(finalPath, readError))
                containsSchemaChanges = written.ContainsSchemaChanges();
        }

        Json entry = Json::object();
        entry["id"] = id;
        entry["index"] = index;
        entry["parentId"] = parentId;
        entry["description"] = a.description.empty()
                                   ? std::string("guid-a-tron: the original changeset, with the guids in it")
                                   : a.description;
        entry["changesType"] = containsSchemaChanges ? 1 : 0;
        entry["bytes"] = bytes;
        entry["pathname"] = finalPath;
        entry["original"] = BaseName(item.second);
        entry["rewrite"] = RewriteCountsJson(counts);
        manifest["changesets"].push_back(entry);

        rc = store.RecordProgress(index, id);
        if (DB_OK == rc)
            rc = store.Flush();
        if (DB_OK != rc) {
            fprintf(stderr, "guid-a-tron: the guid store could not be written: %s\n",
                    DbResultText(rc));
            failed = true;
            break;
        }

        printf("%6lld %s -> %s  %lld filled, %lld delete old value(s) fixed, %lld unresolved, "
               "%llu bytes\n", (long long)index, BaseName(item.second).c_str(), id.c_str(),
               (long long)counts.insertsWithoutGuid, (long long)counts.deletesWithoutGuid,
               (long long)counts.unresolved, (unsigned long long)bytes);
        total.Add(counts);
        parentId = id;
        lastIndex = index;
        ++done;
        // The manifest is rewritten whole, so it is saved in batches rather than per changeset: a
        // run over tens of thousands of changesets would otherwise spend its time on JSON.
        if (done % 500 == 0)
            SaveManifest(a.fork, manifest);
    }

    if (manifest.contains("fork") && manifest["fork"].is_object()) {
        manifest["fork"]["rewrittenToIndex"] = lastIndex;
        manifest["fork"]["rewrittenCount"] = manifest["fork"].value("rewrittenCount", (int64_t)0) + done;
    }
    SaveManifest(a.fork, manifest);
    printf("\n");
    PrintRewriteCounts("total:", total);
    printf("guid store: %lld assignment(s), %lld retired\n", (long long)store.Size(),
           (long long)store.Retired());
    store.Close();
    if (total.unresolved > 0)
        printf("WARNING: %lld NULL guid(s) could not be filled — the store does not cover this "
               "history. Every one of those is a deletion the transformer cannot match by guid.\n",
               (long long)total.unresolved);
    return failed ? 1 : 0;
}

/**
 * `scan` — what a changeset asks of the rewriter, and whether this tool can read it losslessly.
 *
 * Two answers in one pass, neither of which needs a database or writes anything:
 *
 *   - the census: how many inserts arrive without a guid, how many deletes record a NULL **old**
 *     guid (the ones only a rewrite can fix), how many updates touch the column at all.
 *   - the round-trip: the id of the changeset as this tool would re-emit it with no substitutions.
 *     It must equal the original's. The id is SHA1 over the prefix and every decompressed byte in
 *     order, so an equal id means the record reader, the record writer and the prefix handling are
 *     all byte-exact — over as much of the corpus as you care to point it at.
 *
 * Run over a fork's **own** changesets it is the acceptance test: `insertsWithoutGuid` and
 * `deletesWithoutGuid` must both be 0.
 */
int CommandScan(Args const& a) {
    std::vector<std::pair<int64_t, std::string>> work;
    if (!a.changesetList.empty()) {
        if (!LoadChangesetList(a.changesetList, work))
            return 1;
    } else if (!a.changeset.empty()) {
        work.push_back({a.index > 0 ? a.index : 0, a.changeset});
    } else {
        fprintf(stderr, "guid-a-tron scan needs --changeset or --changeset-list\n");
        return 2;
    }

    ElementGuidStore store; // never opened: a scan substitutes nothing and records nothing
    RewriteOptions options;
    options.substitute = false;
    if (a.guidColumn >= 0)
        options.guidColumn = a.guidColumn;

    RewriteCounts total;
    int64_t same = 0, different = 0, unreadable = 0;
    time_t started = time(nullptr);
    for (auto const& item : work) {
        std::string const& file = item.second;
        RewriteCounts counts;
        std::string rewritten;
        std::string error;
        options.index = item.first;
        DbResult rc = RewrittenChangesetId(file, std::string(), store, options, counts, rewritten,
                                           error);
        if (DB_OK != rc) {
            fprintf(stderr, "%lld %s: %s\n", (long long)item.first, item.second.c_str(),
                    Why(rc, error));
            ++unreadable;
            continue;
        }
        std::string originalId = ComputeChangesetId(std::string(), file);
        bool lossless = originalId == rewritten;
        lossless ? ++same : ++different;
        total.Add(counts);
        if (work.size() <= 40 || !lossless || counts.unexpectedGuidValues > 0)
            printf("%6lld %-42s %s  %lld insert(s) and %lld delete old value(s) without a guid\n",
                   (long long)item.first, BaseName(item.second).c_str(),
                   lossless ? "round-trips" : "DIFFERS",
                   (long long)counts.insertsWithoutGuid, (long long)counts.deletesWithoutGuid);
        if ((same + different) % 2000 == 0)
            printf("… %lld changeset(s), %.1f GB, %.0fs\n", (long long)(same + different),
                   (double)total.bytesIn / 1e9, difftime(time(nullptr), started));
    }
    printf("\n%lld changeset(s): %lld round-trip to the same id, %lld differ, %lld unreadable\n",
           (long long)work.size(), (long long)same, (long long)different, (long long)unreadable);
    PrintRewriteCounts("total:", total);
    double seconds = difftime(time(nullptr), started);
    printf("read %.2f GB of decompressed change stream in %.0fs = %.1f MB/s\n",
           (double)total.bytesIn / 1e9, seconds,
           seconds > 0 ? (double)total.bytesIn / 1e6 / seconds : 0.0);
    return (different == 0 && unreadable == 0) ? 0 : 4;
}

int CommandCoverage(Args const& a) {
    if (a.imodel.empty()) {
        fprintf(stderr, "guid-a-tron coverage needs --imodel\n");
        return 2;
    }
    Db db;
    DbResult rc = db.OpenDb(a.imodel, Db::OpenMode::Readonly);
    if (DB_OK != rc)
        return Fail("opening read-only", rc);
    PrintCoverage(a.imodel.c_str(), MeasureCoverage(db));
    db.CloseDb();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    // Unbuffered: these commands run for minutes and one of them crashed once, taking a whole run's
    // output with it. A redirected stdout is block-buffered by default, so a crash loses everything.
    setvbuf(stdout, nullptr, _IONBF, 0);
    Args a = Parse(argc, argv);
    if (!a.ok || a.command == "--help" || a.command == "-h" || a.command == "help") {
        if (!a.error.empty())
            fprintf(stderr, "guid-a-tron: %s\n\n", a.error.c_str());
        fputs(USAGE, a.ok ? stdout : stderr);
        return a.ok ? 0 : 2;
    }
    // Once per process, before any db is opened — and that is the whole of this tool's setup.
    if (!InitializeSqlite())
        return 1;

    if (a.command == "id") return CommandId(a);
    if (a.command == "coverage") return CommandCoverage(a);
    if (a.command == "seed") return CommandSeed(a);
    if (a.command == "rewrite") return CommandRewrite(a);
    if (a.command == "scan") return CommandScan(a);

    fprintf(stderr, "guid-a-tron: unknown command %s\n\n", a.command.c_str());
    fputs(USAGE, stderr);
    return 2;
}
