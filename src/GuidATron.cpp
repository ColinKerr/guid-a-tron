/*---------------------------------------------------------------------------------------------
 * guid-a-tron — implementation. See GuidATron.h for why each piece is shaped the way it is.
 *--------------------------------------------------------------------------------------------*/
#include "GuidATron.h"

#include "Sha1.h"

#include <stdio.h>
#include <string.h>

namespace guidatron {

//---------------------------------------------------------------------------------------------
Coverage MeasureCoverage(Db const& db) {
    Coverage out;
    Statement stmt;
    if (DB_OK != stmt.Prepare(db, "SELECT COUNT(*), SUM(CASE WHEN FederationGuid IS NULL THEN 1 ELSE 0 END),"
                                  " COUNT(DISTINCT FederationGuid) FROM bis_Element"))
        return out;
    if (DB_ROW == stmt.Step()) {
        out.elements = stmt.GetValueInt64(0);
        out.withoutGuid = stmt.GetValueInt64(1);
        out.distinctGuids = stmt.GetValueInt64(2);
    }
    return out;
}

//---------------------------------------------------------------------------------------------
// See GuidATron.h. The incarnation is absent from the hashed string when it is 1.
//---------------------------------------------------------------------------------------------
Guid DeriveElementGuid(GuidCR dbGuid, uint64_t elementId, uint32_t incarnation) {
    std::string seed = incarnation <= 1
                           ? Format("%s|%llu", dbGuid.ToString().c_str(), (unsigned long long)elementId)
                           : Format("%s|%llu|%u", dbGuid.ToString().c_str(),
                                    (unsigned long long)elementId, (unsigned)incarnation);
    Sha1 sha;
    sha.Add(seed.c_str(), seed.size());

    Byte digest[Sha1::HashBytes];
    sha.GetHash(digest);

    Guid guid;
    memcpy(guid.b, digest, sizeof(guid.b)); // the first 16 of the 20 bytes
    guid.b[6] = (Byte)((guid.b[6] & 0x0f) | 0x50); // version 5
    guid.b[8] = (Byte)((guid.b[8] & 0x3f) | 0x80); // RFC 4122 variant
    return guid;
}

/**
 * Triggers off on one connection for a scope, and back to whatever they were afterwards.
 *
 * One caller: GUIDating the seed. `bis_Element_CurrentTimeStamp` fires precisely when an update
 * leaves `LastMod` at the value the row already held —
 *
 *     CREATE TRIGGER [bis_Element_CurrentTimeStamp] AFTER UPDATE ON [bis_Element]
 *       WHEN old.[LastMod]=new.[LastMod] AND old.[LastMod]!=julianday('now')
 *       BEGIN UPDATE [bis_Element] SET [LastMod]=julianday('now') WHERE [Id]=new.[Id]; END
 *
 * — and it does not care whether the column was in the SET clause, so a guid-only write cannot
 * keep a row's `LastMod` by restoring it afterwards. Keeping the trigger out of the statement is
 * the only way, and the seed differing from the baseline in exactly one column is the property the
 * whole design rests on. See ../IMPLEMENTATION.md, *The seed, and LastMod*.
 *
 * A `.bim` also carries spatial-index triggers whose bodies call DgnPlatform's own SQL functions
 * (`dgn_rtree_ins` and friends call `DGN_bbox_value`), which no process but iTwin's own registers.
 * They fire on insert and delete, not on the update the seed performs, so they are not the reason
 * for this — but they are the reason nothing here inserts a row into a geometric table, and the
 * reason nothing here applies a changeset: that is iTwin.js's job, on a connection where those
 * functions exist.
 *
 * Scoped, so a failure path cannot leave a connection with its triggers off.
 */
SuspendTriggers::SuspendTriggers(Db const& db) : m_db(db.GetSqlDb()) {
    if (SQLITE_OK != sqlite3_db_config(m_db, SQLITE_DBCONFIG_ENABLE_TRIGGER, -1, &m_previous))
        return; // could not even read the setting; leave it alone
    m_suspended = SQLITE_OK == sqlite3_db_config(m_db, SQLITE_DBCONFIG_ENABLE_TRIGGER, 0, nullptr);
    if (!m_suspended)
        fprintf(stderr, "guid-a-tron: could not suspend triggers\n");
}

SuspendTriggers::~SuspendTriggers() {
    if (m_suspended)
        sqlite3_db_config(m_db, SQLITE_DBCONFIG_ENABLE_TRIGGER, m_previous, nullptr);
}

} // namespace guidatron
