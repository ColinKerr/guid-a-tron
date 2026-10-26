/*---------------------------------------------------------------------------------------------
 * guid-a-tron — give a real iModel's history `federationGuid`s without changing anything else.
 *
 * The support an element-guid rewrite needs around it: the guid derivation, the coverage census
 * and the trigger suspension. The rewrite itself is in ChangesetRewrite.h, the format it reads and
 * writes is in ChangesetRecords.h, and the container around it is in ChangesetFile.h.
 *
 * Why the guids have to be in the history and not merely in the file: a transformer reads a
 * deleted element's guid **out of the changeset** (`ChangesetScanner.js:120-131` →
 * `IModelTransformer.js:1549-1572`), so a briefcase whose rows carry guids is not enough. See
 * ../IMPLEMENTATION.md.
 *--------------------------------------------------------------------------------------------*/
#pragma once

#include "Guid.h"
#include "Sqlite.h"

#include <string>

namespace guidatron {

//! How a guid is chosen for an element that has none.
enum class GuidPolicy {
    //! A fresh random guid, which is what the platform does for an element inserted without one
    //! (measured on core-backend 5.14.2: `insertElement` assigns one). The production-faithful
    //! choice, and not reproducible across rebuilds.
    Random,
    //! SHA-1 over the file's own DbGuid, the element id and its incarnation, with the UUID version
    //! and variant bits set. Reproducible, so a fork can be rebuilt rather than archived.
    Derived,
};

/**
 * A guid derived from (scope, element id, incarnation), shaped like a UUIDv5: the first 16 bytes of
 * a SHA-1 over the three, with the version nibble set to 5 and the variant bits to RFC 4122.
 *
 * The incarnation is what makes it safe in the face of element ids a history deletes and later
 * re-inserts (`ChangesetRewrite.h`): two elements that share an id get different guids. It is left
 * out of the hashed string when it is 1.
 */
Guid DeriveElementGuid(GuidCR scope, uint64_t elementId, uint32_t incarnation = 1);

//! What one GUIDating pass over a seed did.
struct SeedCounts {
    int64_t elementsConsidered = 0; //!< rows with a NULL FederationGuid before the pass
    int64_t elementsGuidated = 0;   //!< rows this pass wrote a guid onto
    //! Rows whose LastMod moved — the `bis_Element_CurrentTimeStamp` trigger getting into a write
    //! it should be kept out of. Expected to be 0; reported rather than assumed, because the seed
    //! is meant to differ from the baseline in exactly one column.
    int64_t lastModUnstable = 0;
};

//! What `bis_Element` holds, counted — the before/after check of a seed and of a replay.
struct Coverage {
    int64_t elements = 0;
    int64_t withoutGuid = 0;
    int64_t distinctGuids = 0;
};

Coverage MeasureCoverage(Db const& db);

/**
 * Triggers off on one connection for the lifetime of this object, and back as they were after.
 *
 * One use: GUIDating a seed, because `bis_Element_CurrentTimeStamp` would otherwise bump `LastMod`
 * on every row it touches, and the seed is meant to differ from the baseline in exactly one column.
 * See the definition in GuidATron.cpp for why restoring `LastMod` afterwards is not an option.
 */
struct SuspendTriggers {
    explicit SuspendTriggers(Db const& db);
    ~SuspendTriggers();
    SuspendTriggers(SuspendTriggers const&) = delete;
    SuspendTriggers& operator=(SuspendTriggers const&) = delete;

    bool IsSuspended() const { return m_suspended; }

private:
    sqlite3* m_db = nullptr;
    int m_previous = 1;
    bool m_suspended = false;
};

} // namespace guidatron
