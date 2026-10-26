/*---------------------------------------------------------------------------------------------
 * guid-a-tron — the database.
 *--------------------------------------------------------------------------------------------*/
#include "Sqlite.h"

#include "Files.h"

#include <stdio.h>
#include <string.h>

namespace guidatron {

//---------------------------------------------------------------------------------------------
char const* DbResultText(DbResult result) { return sqlite3_errstr((int)result); }

//---------------------------------------------------------------------------------------------
// The whole of this tool's process setup. SQLite needs a temp directory only when it spills to
// one, and finds it itself — this sets it
// explicitly anyway, so a sandbox with no TMPDIR behaves the same as a desktop.
//---------------------------------------------------------------------------------------------
bool InitializeSqlite() {
    int rc = sqlite3_initialize();
    if (SQLITE_OK != rc) {
        fprintf(stderr, "guid-a-tron: sqlite3_initialize failed: %s\n", sqlite3_errstr(rc));
        return false;
    }
    static std::string tempDir = TempDir();
    sqlite3_temp_directory = (char*)tempDir.c_str();
    return true;
}

//=============================================================================================
// Db
//=============================================================================================

//---------------------------------------------------------------------------------------------
DbResult Db::Begin() {
    if (m_inTxn)
        return DB_OK;
    DbResult rc = ExecuteSql("BEGIN");
    if (DB_OK == rc)
        m_inTxn = true;
    return rc;
}

//---------------------------------------------------------------------------------------------
DbResult Db::OpenDb(std::string const& path, OpenMode mode) {
    if (IsDbOpen())
        return DB_ERROR;
    if (!FileExists(path))
        return DB_NOTFOUND;
    int flags = mode == OpenMode::Readonly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE;
    int rc = sqlite3_open_v2(path.c_str(), &m_db, flags, nullptr);
    if (SQLITE_OK != rc) {
        sqlite3_close_v2(m_db);
        m_db = nullptr;
        return (DbResult)rc;
    }
    sqlite3_busy_timeout(m_db, 5000); // the same wait BeSQLite installs by default
    if (mode == OpenMode::ReadWrite) {
        DbResult begun = Begin();
        if (DB_OK != begun) {
            CloseDb();
            return begun;
        }
    }
    return DB_OK;
}

//---------------------------------------------------------------------------------------------
DbResult Db::CreateNewDb(std::string const& path) {
    if (IsDbOpen())
        return DB_ERROR;
    if (FileExists(path))
        return DB_ERROR; // never overwrite: the caller's input may be irreplaceable
    if (!CreateDirectories(ParentDir(path)))
        return DB_ERROR;
    int rc = sqlite3_open_v2(path.c_str(), &m_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                             nullptr);
    if (SQLITE_OK != rc) {
        sqlite3_close_v2(m_db);
        m_db = nullptr;
        return (DbResult)rc;
    }
    sqlite3_busy_timeout(m_db, 5000);
    DbResult begun = Begin();
    if (DB_OK != begun) {
        CloseDb();
        return begun;
    }
    return DB_OK;
}

//---------------------------------------------------------------------------------------------
DbResult Db::SaveChanges(char const*) {
    if (!IsDbOpen())
        return DB_ERROR;
    if (m_inTxn) {
        DbResult rc = ExecuteSql("COMMIT");
        if (DB_OK != rc)
            return rc;
        m_inTxn = false;
    }
    return Begin();
}

//---------------------------------------------------------------------------------------------
void Db::CloseDb() {
    if (!IsDbOpen())
        return;
    if (m_inTxn) {
        // Whatever has not been saved is abandoned, as it is with BeSQLite's default transaction.
        // Every writer here calls SaveChanges explicitly, so this only ever discards a failed run.
        ExecuteSql("ROLLBACK");
        m_inTxn = false;
    }
    sqlite3_close_v2(m_db); // checkpoints and removes the -wal when this is the last connection
    m_db = nullptr;
}

//---------------------------------------------------------------------------------------------
DbResult Db::ExecuteSql(char const* sql) {
    if (!IsDbOpen())
        return DB_ERROR;
    char* message = nullptr;
    int rc = sqlite3_exec(m_db, sql, nullptr, nullptr, &message);
    if (SQLITE_OK != rc && message != nullptr)
        fprintf(stderr, "guid-a-tron: %s\n", message);
    sqlite3_free(message);
    return (DbResult)rc;
}

//---------------------------------------------------------------------------------------------
// `be_Prop`'s primary key is (Namespace, Name, Id, SubId), all BINARY — it is not one of the 21
// schema objects that use NOCASE — so this is an exact seek on the key.
//---------------------------------------------------------------------------------------------
Guid Db::GetDbGuid() const {
    Guid guid;
    Statement stmt;
    if (DB_OK != stmt.Prepare(*this, "SELECT Data FROM be_Prop WHERE Namespace='be_Db'"
                                     " AND Name='DbGuid' AND Id=0 AND SubId=0"))
        return guid;
    if (DB_ROW == stmt.Step())
        guid = stmt.GetValueGuid(0);
    return guid;
}

//---------------------------------------------------------------------------------------------
// `COLLATE BINARY` on purpose: `be_Local.Name` is declared `COLLATE NOCASE UNIQUE`, and forcing
// BINARY means this cannot seek that index at all. It scans instead — `be_Local` holds six rows in
// a real briefcase — so the answer does not depend on which `NOCASE` ordered the index. The names
// this tool asks for (`ParentChangeSetId`, `parentChangeset`) are stored with exactly that casing.
//---------------------------------------------------------------------------------------------
bool Db::QueryBriefcaseLocalValue(std::string& value, char const* name) const {
    value.clear();
    Statement stmt;
    if (DB_OK != stmt.Prepare(*this, "SELECT Val FROM be_Local WHERE Name=? COLLATE BINARY"))
        return false; // no be_Local: not a briefcase, which is a fair answer rather than an error
    stmt.BindText(1, name);
    if (DB_ROW != stmt.Step())
        return false;
    value = stmt.GetValueText(0);
    return true;
}

//=============================================================================================
// Statement
//=============================================================================================

//---------------------------------------------------------------------------------------------
DbResult Statement::Prepare(Db const& db, char const* sql) {
    Finalize();
    if (!db.IsDbOpen())
        return DB_ERROR;
    int rc = sqlite3_prepare_v2(db.GetSqlDb(), sql, -1, &m_stmt, nullptr);
    if (SQLITE_OK != rc) {
        fprintf(stderr, "guid-a-tron: %s\n  in: %s\n", sqlite3_errmsg(db.GetSqlDb()), sql);
        m_stmt = nullptr;
    }
    return (DbResult)rc;
}

void Statement::Finalize() {
    if (m_stmt != nullptr)
        sqlite3_finalize(m_stmt);
    m_stmt = nullptr;
}

DbResult Statement::Step() { return m_stmt == nullptr ? DB_ERROR : (DbResult)sqlite3_step(m_stmt); }
DbResult Statement::Reset() { return m_stmt == nullptr ? DB_ERROR : (DbResult)sqlite3_reset(m_stmt); }
DbResult Statement::ClearBindings() {
    return m_stmt == nullptr ? DB_ERROR : (DbResult)sqlite3_clear_bindings(m_stmt);
}

DbResult Statement::BindInt(int parameter, int value) {
    return m_stmt == nullptr ? DB_ERROR : (DbResult)sqlite3_bind_int(m_stmt, parameter, value);
}
DbResult Statement::BindInt64(int parameter, int64_t value) {
    return m_stmt == nullptr ? DB_ERROR
                             : (DbResult)sqlite3_bind_int64(m_stmt, parameter, (sqlite3_int64)value);
}
//! An element id is unsigned but SQLite integers are signed; the bits are what matter, and they
//! survive the round trip, which is what `GetValueUInt64` relies on.
DbResult Statement::BindUInt64(int parameter, uint64_t value) {
    return BindInt64(parameter, (int64_t)value);
}
DbResult Statement::BindDouble(int parameter, double value) {
    return m_stmt == nullptr ? DB_ERROR : (DbResult)sqlite3_bind_double(m_stmt, parameter, value);
}
DbResult Statement::BindNull(int parameter) {
    return m_stmt == nullptr ? DB_ERROR : (DbResult)sqlite3_bind_null(m_stmt, parameter);
}
DbResult Statement::BindText(int parameter, char const* value) {
    if (m_stmt == nullptr)
        return DB_ERROR;
    return (DbResult)sqlite3_bind_text(m_stmt, parameter, value, -1, SQLITE_TRANSIENT);
}
DbResult Statement::BindGuid(int parameter, GuidCR guid) {
    if (m_stmt == nullptr)
        return DB_ERROR;
    return (DbResult)sqlite3_bind_blob(m_stmt, parameter, guid.b, (int)sizeof(guid.b),
                                       SQLITE_TRANSIENT);
}

int Statement::GetValueInt(int column) const {
    return m_stmt == nullptr ? 0 : sqlite3_column_int(m_stmt, column);
}
int64_t Statement::GetValueInt64(int column) const {
    return m_stmt == nullptr ? 0 : (int64_t)sqlite3_column_int64(m_stmt, column);
}
uint64_t Statement::GetValueUInt64(int column) const { return (uint64_t)GetValueInt64(column); }
double Statement::GetValueDouble(int column) const {
    return m_stmt == nullptr ? 0.0 : sqlite3_column_double(m_stmt, column);
}
char const* Statement::GetValueText(int column) const {
    if (m_stmt == nullptr)
        return "";
    char const* text = (char const*)sqlite3_column_text(m_stmt, column);
    return text == nullptr ? "" : text;
}
Guid Statement::GetValueGuid(int column) const {
    Guid guid;
    if (m_stmt == nullptr)
        return guid;
    void const* bytes = sqlite3_column_blob(m_stmt, column);
    if (bytes != nullptr && sqlite3_column_bytes(m_stmt, column) == (int)sizeof(guid.b))
        memcpy(guid.b, bytes, sizeof(guid.b));
    return guid;
}
bool Statement::IsColumnNull(int column) const {
    return m_stmt == nullptr || SQLITE_NULL == sqlite3_column_type(m_stmt, column);
}

} // namespace guidatron
