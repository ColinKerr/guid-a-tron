/*---------------------------------------------------------------------------------------------
 * guid-a-tron — the database, over SQLite's own C API.
 *
 * SQLite is compiled here from the public amalgamation (see CMakeLists.txt), and this file is the
 * whole of what this tool needs in place of BeSQLite: open a file, run statements, commit. What it
 * deliberately does **not** do is everything else `BeSQLite::Db` does on the way in — no profile
 * check, no profile upgrade, no `be_Prop` bookkeeping, no custom SQL functions — because this tool
 * reads a briefcase and writes one column of it, and a tool that silently upgrades its input is a
 * tool you cannot trust with a baseline.
 *
 *--------------------------------------------------------------------------------------------*/
#pragma once

#include "Guid.h"
#include "Types.h"

#include "sqlite3.h"

#include <string>

namespace guidatron {

//! SQLite's own result codes, under names that say they are this tool's.
enum DbResult : int {
    DB_OK = SQLITE_OK,
    DB_ERROR = SQLITE_ERROR,
    DB_MISMATCH = SQLITE_MISMATCH,
    DB_NOTFOUND = SQLITE_CANTOPEN, //!< where a file that must exist does not
    DB_ROW = SQLITE_ROW,
    DB_DONE = SQLITE_DONE,
};

//! SQLite's description of a result code, for an error message.
char const* DbResultText(DbResult result);

//! SQLite's process-wide setup. Called once, before any db is opened.
bool InitializeSqlite();

struct Statement;

struct Db {
    enum class OpenMode { Readonly, ReadWrite };

    Db() {}
    ~Db() { CloseDb(); }
    Db(Db const&) = delete;
    Db& operator=(Db const&) = delete;

    //! Open an existing file. Readonly is what every command but `seed` uses on its input.
    DbResult OpenDb(std::string const& path, OpenMode mode);
    //! Create a file that must not already exist. Used for the guid store, and nothing else.
    DbResult CreateNewDb(std::string const& path);
    //! Commit what is pending and begin the next transaction. The message is for the caller's log.
    DbResult SaveChanges(char const* message = nullptr);
    //! Roll back anything unsaved and close.
    void CloseDb();
    bool IsDbOpen() const { return m_db != nullptr; }
    DbResult ExecuteSql(char const* sql);

    //! The file's own `be_Db/DbGuid` property, which is the scope a derived guid is derived in.
    Guid GetDbGuid() const;
    //! A `be_Local` row, e.g. `ParentChangeSetId`. False when the table or the row is absent.
    bool QueryBriefcaseLocalValue(std::string& value, char const* name) const;

    sqlite3* GetSqlDb() const { return m_db; }

private:
    DbResult Begin();
    sqlite3* m_db = nullptr;
    bool m_inTxn = false;
};

/**
 * A prepared statement.
 *
 * Bind parameters are 1-based and columns 0-based, which is SQLite's own convention. `BindText`
 * always copies: every caller here binds a short-lived string.
 */
struct Statement {
    Statement() {}
    ~Statement() { Finalize(); }
    Statement(Statement const&) = delete;
    Statement& operator=(Statement const&) = delete;

    DbResult Prepare(Db const& db, char const* sql);
    void Finalize();
    bool IsPrepared() const { return m_stmt != nullptr; }

    //! DB_ROW, DB_DONE, or an error.
    DbResult Step();
    DbResult Reset();
    DbResult ClearBindings();

    DbResult BindInt(int parameter, int value);
    DbResult BindInt64(int parameter, int64_t value);
    DbResult BindUInt64(int parameter, uint64_t value);
    DbResult BindDouble(int parameter, double value);
    DbResult BindNull(int parameter);
    DbResult BindText(int parameter, char const* value);
    DbResult BindText(int parameter, std::string const& value) { return BindText(parameter, value.c_str()); }
    //! A guid is a 16-byte blob, which is how `bis_Element.FederationGuid` holds one.
    DbResult BindGuid(int parameter, GuidCR guid);

    int GetValueInt(int column) const;
    int64_t GetValueInt64(int column) const;
    uint64_t GetValueUInt64(int column) const;
    double GetValueDouble(int column) const;
    //! Valid until the next Step or Reset, as SQLite's own is. Never null: "" for a NULL column.
    char const* GetValueText(int column) const;
    //! Invalid — all zeros — unless the column holds exactly 16 bytes.
    Guid GetValueGuid(int column) const;
    bool IsColumnNull(int column) const;

private:
    sqlite3_stmt* m_stmt = nullptr;
};

} // namespace guidatron
