// components/pscal-core/plugins/sqlite/sqlite_ext_plugin.c
//
// VM 2.0 Phase 7 (Docs/pscal_vm2_plan.md §7.1): the "prove it" deliverable --
// the sqlite category re-implemented as a dlopen'd plugin, built as a
// separate .dylib/.so and loaded via --ext, registering the same sqlite*
// builtins the in-tree static category (src/ext_builtins/sqlite/
// sqlite_builtins.c) does. That file is UNTOUCHED; both coexist (the static
// category is what ships by default, this plugin exists only to validate
// the ABI end-to-end).
//
// Deliberately includes ONLY backend_ast/pscal_ext_api.h from pscal-core --
// no core/types.h, no backend_ast/builtin.h, no core/utils.h. Every host
// interaction goes through the g_host vtable captured in pscal_ext_register().
// This is what makes it a genuine proof of the ABI rather than a recompiled
// copy of privileged code: a real third-party plugin author would write
// exactly this shape, working from nothing but the frozen header.
//
// One behavioral difference from the static category, by design: the
// generic host handle table (pscal_ext_api.h's PscalExtHandleTable) doesn't
// support enumerating all handles of a kind, so SqliteClose's "finalize
// every open statement belonging to this db" cascade is implemented here
// with a small plugin-local side table (g_stmt_owners) instead of scanning
// the host's handle table directly (which is how the static version does
// it, since it owns its handle table outright). This is a deliberate,
// documented scope choice for the generic handle-table helper (see
// pscal_ext_api.h's design comment) rather than growing the ABI surface
// for one call's cleanup behavior.
#include "backend_ast/pscal_ext_api.h"

#include <sqlite3.h>

#include <stdlib.h>
#include <string.h>

static const PscalExtHostApi *g_host = NULL;
static PscalExtHandleTable *g_handles = NULL;

typedef enum {
    SQLITE_PLUGIN_HANDLE_DB = 1,
    SQLITE_PLUGIN_HANDLE_STATEMENT = 2
} SqlitePluginHandleKind;

// Side table mapping an open statement handle to the db handle that owns
// it, purely so SqliteClose can finalize orphaned statements the same way
// the static category does (see file header comment above).
typedef struct {
    int stmt_handle;
    int db_handle;
    bool active;
} StmtOwnerEntry;

static StmtOwnerEntry *g_stmt_owners = NULL;
static size_t g_stmt_owner_count = 0;
static size_t g_stmt_owner_capacity = 0;

static void trackStmtOwner(int stmt_handle, int db_handle) {
    if (g_stmt_owner_count == g_stmt_owner_capacity) {
        size_t new_capacity = g_stmt_owner_capacity ? g_stmt_owner_capacity * 2 : 16;
        StmtOwnerEntry *grown = (StmtOwnerEntry *)realloc(g_stmt_owners, new_capacity * sizeof(StmtOwnerEntry));
        if (!grown) {
            return;
        }
        g_stmt_owners = grown;
        g_stmt_owner_capacity = new_capacity;
    }
    g_stmt_owners[g_stmt_owner_count].stmt_handle = stmt_handle;
    g_stmt_owners[g_stmt_owner_count].db_handle = db_handle;
    g_stmt_owners[g_stmt_owner_count].active = true;
    g_stmt_owner_count++;
}

static void untrackStmtOwner(int stmt_handle) {
    for (size_t i = 0; i < g_stmt_owner_count; ++i) {
        if (g_stmt_owners[i].active && g_stmt_owners[i].stmt_handle == stmt_handle) {
            g_stmt_owners[i].active = false;
            return;
        }
    }
}

// Finalizes and untracks every still-open statement owned by `db_handle`,
// mirroring vmSqliteClose's cascade in the static category.
static void finalizeOrphanedStatements(int db_handle) {
    for (size_t i = 0; i < g_stmt_owner_count; ++i) {
        if (!g_stmt_owners[i].active || g_stmt_owners[i].db_handle != db_handle) {
            continue;
        }
        void *payload = g_host->handle_free(g_handles, g_stmt_owners[i].stmt_handle);
        if (payload) {
            sqlite3_finalize((sqlite3_stmt *)payload);
        }
        g_stmt_owners[i].active = false;
    }
}

static const char *sqliteTypeToString(int type) {
    switch (type) {
        case SQLITE_INTEGER: return "INTEGER";
        case SQLITE_FLOAT:   return "FLOAT";
        case SQLITE_TEXT:    return "TEXT";
        case SQLITE_BLOB:    return "BLOB";
        case SQLITE_NULL:    return "NULL";
        default:             return "UNKNOWN";
    }
}

static Value pluginSqliteOpen(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1) {
        g_host->runtime_error(vm, "SqliteOpen expects exactly 1 argument.");
        return g_host->make_int(-1);
    }
    if (!g_host->is_string_type(args[0].type)) {
        g_host->runtime_error(vm, "SqliteOpen argument must be a string path.");
        return g_host->make_int(-1);
    }
    const char *path = g_host->as_cstring(args[0]);
    if (!path) {
        path = ":memory:";
    }
    sqlite3 *db = NULL;
    int rc = sqlite3_open(path, &db);
    if (rc != SQLITE_OK) {
        const char *msg = db ? sqlite3_errmsg(db) : "sqlite3_open failed";
        g_host->runtime_error(vm, "SqliteOpen failed (%d): %s", rc, msg ? msg : "unknown");
        if (db) {
            sqlite3_close(db);
        }
        return g_host->make_int(-1);
    }
    int handle = g_host->handle_alloc(g_handles, SQLITE_PLUGIN_HANDLE_DB, db);
    if (handle < 0) {
        g_host->runtime_error(vm, "SqliteOpen: unable to allocate handle.");
        sqlite3_close(db);
        return g_host->make_int(-1);
    }
    return g_host->make_int(handle);
}

static Value pluginSqliteClose(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1) {
        g_host->runtime_error(vm, "SqliteClose expects exactly 1 argument.");
        return g_host->make_int(-1);
    }
    if (!g_host->is_intlike_type(args[0].type)) {
        g_host->runtime_error(vm, "SqliteClose argument must be an integer handle.");
        return g_host->make_int(-1);
    }
    int handle = (int)g_host->as_int64(args[0]);
    void *payload = NULL;
    if (!g_host->handle_lookup(g_handles, handle, SQLITE_PLUGIN_HANDLE_DB, &payload) || !payload) {
        g_host->runtime_error(vm, "SqliteClose received invalid database handle %d.", handle);
        return g_host->make_int(-1);
    }
    finalizeOrphanedStatements(handle);
    sqlite3 *db = (sqlite3 *)g_host->handle_free(g_handles, handle);
    int rc = sqlite3_close(db);
    if (rc != SQLITE_OK) {
        g_host->runtime_error(vm, "SqliteClose failed (%d): %s", rc, sqlite3_errmsg(db));
    }
    return g_host->make_int(rc);
}

static Value pluginSqliteExec(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 2) {
        g_host->runtime_error(vm, "SqliteExec expects (db_handle:int, sql:string).");
        return g_host->make_int(-1);
    }
    if (!g_host->is_intlike_type(args[0].type) || !g_host->is_string_type(args[1].type)) {
        g_host->runtime_error(vm, "SqliteExec argument types are (int, string).");
        return g_host->make_int(-1);
    }
    void *payload = NULL;
    if (!g_host->handle_lookup(g_handles, (int)g_host->as_int64(args[0]), SQLITE_PLUGIN_HANDLE_DB, &payload) ||
        !payload) {
        g_host->runtime_error(vm, "SqliteExec received invalid database handle %d.", (int)g_host->as_int64(args[0]));
        return g_host->make_int(-1);
    }
    const char *sql = g_host->as_cstring(args[1]);
    if (!sql) {
        g_host->runtime_error(vm, "SqliteExec received NIL SQL string.");
        return g_host->make_int(-1);
    }
    char *err_msg = NULL;
    int rc = sqlite3_exec((sqlite3 *)payload, sql, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        g_host->runtime_error(vm, "SqliteExec failed (%d): %s", rc, err_msg ? err_msg : "unknown");
    }
    if (err_msg) {
        sqlite3_free(err_msg);
    }
    return g_host->make_int(rc);
}

static Value pluginSqlitePrepare(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 2) {
        g_host->runtime_error(vm, "SqlitePrepare expects (db_handle:int, sql:string).");
        return g_host->make_int(-1);
    }
    if (!g_host->is_intlike_type(args[0].type) || !g_host->is_string_type(args[1].type)) {
        g_host->runtime_error(vm, "SqlitePrepare argument types are (int, string).");
        return g_host->make_int(-1);
    }
    int db_handle = (int)g_host->as_int64(args[0]);
    void *db_payload = NULL;
    if (!g_host->handle_lookup(g_handles, db_handle, SQLITE_PLUGIN_HANDLE_DB, &db_payload) || !db_payload) {
        g_host->runtime_error(vm, "SqlitePrepare received invalid database handle %d.", db_handle);
        return g_host->make_int(-1);
    }
    const char *sql = g_host->as_cstring(args[1]);
    if (!sql) {
        g_host->runtime_error(vm, "SqlitePrepare received NIL SQL string.");
        return g_host->make_int(-1);
    }
    sqlite3 *db = (sqlite3 *)db_payload;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK || !stmt) {
        g_host->runtime_error(vm, "SqlitePrepare failed (%d): %s", rc, sqlite3_errmsg(db));
        return g_host->make_int(-1);
    }
    int stmt_handle = g_host->handle_alloc(g_handles, SQLITE_PLUGIN_HANDLE_STATEMENT, stmt);
    if (stmt_handle < 0) {
        sqlite3_finalize(stmt);
        g_host->runtime_error(vm, "SqlitePrepare: unable to allocate statement handle.");
        return g_host->make_int(-1);
    }
    trackStmtOwner(stmt_handle, db_handle);
    return g_host->make_int(stmt_handle);
}

static Value pluginSqliteFinalize(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1) {
        g_host->runtime_error(vm, "SqliteFinalize expects exactly 1 argument.");
        return g_host->make_int(-1);
    }
    if (!g_host->is_intlike_type(args[0].type)) {
        g_host->runtime_error(vm, "SqliteFinalize argument must be an integer handle.");
        return g_host->make_int(-1);
    }
    int handle = (int)g_host->as_int64(args[0]);
    void *payload = NULL;
    if (!g_host->handle_lookup(g_handles, handle, SQLITE_PLUGIN_HANDLE_STATEMENT, &payload) || !payload) {
        g_host->runtime_error(vm, "SqliteFinalize received invalid statement handle %d.", handle);
        return g_host->make_int(-1);
    }
    g_host->handle_free(g_handles, handle);
    untrackStmtOwner(handle);
    int rc = sqlite3_finalize((sqlite3_stmt *)payload);
    if (rc != SQLITE_OK) {
        g_host->runtime_error(vm, "SqliteFinalize failed (%d).", rc);
    }
    return g_host->make_int(rc);
}

static bool fetchStatement(struct VM_s *vm, Value *args, int count, sqlite3_stmt **out_stmt) {
    if (count < 1) {
        g_host->runtime_error(vm, "SQLite statement operation missing handle argument.");
        return false;
    }
    if (!g_host->is_intlike_type(args[0].type)) {
        g_host->runtime_error(vm, "SQLite statement handle must be an integer.");
        return false;
    }
    void *payload = NULL;
    int handle = (int)g_host->as_int64(args[0]);
    if (!g_host->handle_lookup(g_handles, handle, SQLITE_PLUGIN_HANDLE_STATEMENT, &payload) || !payload) {
        g_host->runtime_error(vm, "SQLite operation received invalid statement handle %d.", handle);
        return false;
    }
    if (out_stmt) {
        *out_stmt = (sqlite3_stmt *)payload;
    }
    return true;
}

static bool fetchDatabase(struct VM_s *vm, Value *args, int count, sqlite3 **out_db) {
    if (count < 1) {
        g_host->runtime_error(vm, "SQLite database operation missing handle argument.");
        return false;
    }
    if (!g_host->is_intlike_type(args[0].type)) {
        g_host->runtime_error(vm, "SQLite database handle must be an integer.");
        return false;
    }
    void *payload = NULL;
    int handle = (int)g_host->as_int64(args[0]);
    if (!g_host->handle_lookup(g_handles, handle, SQLITE_PLUGIN_HANDLE_DB, &payload) || !payload) {
        g_host->runtime_error(vm, "SQLite operation received invalid database handle %d.", handle);
        return false;
    }
    if (out_db) {
        *out_db = (sqlite3 *)payload;
    }
    return true;
}

static Value pluginSqliteStep(struct VM_s *vm, int arg_count, Value *args) {
    sqlite3_stmt *stmt = NULL;
    if (!fetchStatement(vm, args, arg_count, &stmt)) {
        return g_host->make_int(-1);
    }
    return g_host->make_int(sqlite3_step(stmt));
}

static Value pluginSqliteReset(struct VM_s *vm, int arg_count, Value *args) {
    sqlite3_stmt *stmt = NULL;
    if (!fetchStatement(vm, args, arg_count, &stmt)) {
        return g_host->make_int(-1);
    }
    return g_host->make_int(sqlite3_reset(stmt));
}

static bool validateColumnIndex(struct VM_s *vm, sqlite3_stmt *stmt, int column) {
    if (column < 0 || column >= sqlite3_column_count(stmt)) {
        g_host->runtime_error(vm, "SQLite column index %d out of range.", column);
        return false;
    }
    return true;
}

static Value pluginSqliteColumnCount(struct VM_s *vm, int arg_count, Value *args) {
    sqlite3_stmt *stmt = NULL;
    if (!fetchStatement(vm, args, arg_count, &stmt)) {
        return g_host->make_int(-1);
    }
    return g_host->make_int(sqlite3_column_count(stmt));
}

static Value pluginSqliteColumnType(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 2) {
        g_host->runtime_error(vm, "SqliteColumnType expects (stmt_handle:int, column:int).");
        return g_host->make_string("");
    }
    sqlite3_stmt *stmt = NULL;
    if (!fetchStatement(vm, args, arg_count, &stmt)) {
        return g_host->make_string("");
    }
    if (!g_host->is_intlike_type(args[1].type)) {
        g_host->runtime_error(vm, "SqliteColumnType column index must be integer.");
        return g_host->make_string("");
    }
    int column = (int)g_host->as_int64(args[1]);
    if (!validateColumnIndex(vm, stmt, column)) {
        return g_host->make_string("");
    }
    return g_host->make_string(sqliteTypeToString(sqlite3_column_type(stmt, column)));
}

static Value pluginSqliteColumnName(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 2) {
        g_host->runtime_error(vm, "SqliteColumnName expects (stmt_handle:int, column:int).");
        return g_host->make_string("");
    }
    sqlite3_stmt *stmt = NULL;
    if (!fetchStatement(vm, args, arg_count, &stmt)) {
        return g_host->make_string("");
    }
    if (!g_host->is_intlike_type(args[1].type)) {
        g_host->runtime_error(vm, "SqliteColumnName column index must be integer.");
        return g_host->make_string("");
    }
    int column = (int)g_host->as_int64(args[1]);
    if (!validateColumnIndex(vm, stmt, column)) {
        return g_host->make_string("");
    }
    const char *name = sqlite3_column_name(stmt, column);
    return g_host->make_string(name ? name : "");
}

static Value pluginSqliteColumnInt(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 2) {
        g_host->runtime_error(vm, "SqliteColumnInt expects (stmt_handle:int, column:int).");
        return g_host->make_int64(0);
    }
    sqlite3_stmt *stmt = NULL;
    if (!fetchStatement(vm, args, arg_count, &stmt)) {
        return g_host->make_int64(0);
    }
    if (!g_host->is_intlike_type(args[1].type)) {
        g_host->runtime_error(vm, "SqliteColumnInt column index must be integer.");
        return g_host->make_int64(0);
    }
    int column = (int)g_host->as_int64(args[1]);
    if (!validateColumnIndex(vm, stmt, column)) {
        return g_host->make_int64(0);
    }
    return g_host->make_int64((long long)sqlite3_column_int64(stmt, column));
}

static Value pluginSqliteColumnDouble(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 2) {
        g_host->runtime_error(vm, "SqliteColumnDouble expects (stmt_handle:int, column:int).");
        return g_host->make_double(0.0);
    }
    sqlite3_stmt *stmt = NULL;
    if (!fetchStatement(vm, args, arg_count, &stmt)) {
        return g_host->make_double(0.0);
    }
    if (!g_host->is_intlike_type(args[1].type)) {
        g_host->runtime_error(vm, "SqliteColumnDouble column index must be integer.");
        return g_host->make_double(0.0);
    }
    int column = (int)g_host->as_int64(args[1]);
    if (!validateColumnIndex(vm, stmt, column)) {
        return g_host->make_double(0.0);
    }
    return g_host->make_double(sqlite3_column_double(stmt, column));
}

static Value pluginSqliteColumnText(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 2) {
        g_host->runtime_error(vm, "SqliteColumnText expects (stmt_handle:int, column:int).");
        return g_host->make_string("");
    }
    sqlite3_stmt *stmt = NULL;
    if (!fetchStatement(vm, args, arg_count, &stmt)) {
        return g_host->make_string("");
    }
    if (!g_host->is_intlike_type(args[1].type)) {
        g_host->runtime_error(vm, "SqliteColumnText column index must be integer.");
        return g_host->make_string("");
    }
    int column = (int)g_host->as_int64(args[1]);
    if (!validateColumnIndex(vm, stmt, column)) {
        return g_host->make_string("");
    }
    const unsigned char *text = sqlite3_column_text(stmt, column);
    int len = sqlite3_column_bytes(stmt, column);
    if (!text || len <= 0) {
        return g_host->make_string("");
    }
    return g_host->make_string_len((const char *)text, (size_t)len);
}

static Value pluginSqliteBindText(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 3) {
        g_host->runtime_error(vm, "SqliteBindText expects (stmt_handle:int, index:int, value:string).");
        return g_host->make_int(-1);
    }
    sqlite3_stmt *stmt = NULL;
    if (!fetchStatement(vm, args, arg_count, &stmt)) {
        return g_host->make_int(-1);
    }
    if (!g_host->is_intlike_type(args[1].type)) {
        g_host->runtime_error(vm, "SqliteBindText index must be integer.");
        return g_host->make_int(-1);
    }
    if (!g_host->is_string_type(args[2].type)) {
        g_host->runtime_error(vm, "SqliteBindText value must be string.");
        return g_host->make_int(-1);
    }
    int index = (int)g_host->as_int64(args[1]);
    if (index <= 0) {
        g_host->runtime_error(vm, "SqliteBindText parameter index must be >= 1.");
        return g_host->make_int(-1);
    }
    const char *text = g_host->as_cstring(args[2]);
    int rc = sqlite3_bind_text(stmt, index, text ? text : "", -1, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        g_host->runtime_error(vm, "SqliteBindText failed (%d).", rc);
    }
    return g_host->make_int(rc);
}

static Value pluginSqliteBindInt(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 3) {
        g_host->runtime_error(vm, "SqliteBindInt expects (stmt_handle:int, index:int, value:int).");
        return g_host->make_int(-1);
    }
    sqlite3_stmt *stmt = NULL;
    if (!fetchStatement(vm, args, arg_count, &stmt)) {
        return g_host->make_int(-1);
    }
    if (!g_host->is_intlike_type(args[1].type) || !g_host->is_intlike_type(args[2].type)) {
        g_host->runtime_error(vm, "SqliteBindInt arguments must be integers.");
        return g_host->make_int(-1);
    }
    int index = (int)g_host->as_int64(args[1]);
    if (index <= 0) {
        g_host->runtime_error(vm, "SqliteBindInt parameter index must be >= 1.");
        return g_host->make_int(-1);
    }
    int rc = sqlite3_bind_int64(stmt, index, (sqlite3_int64)g_host->as_int64(args[2]));
    if (rc != SQLITE_OK) {
        g_host->runtime_error(vm, "SqliteBindInt failed (%d).", rc);
    }
    return g_host->make_int(rc);
}

static Value pluginSqliteBindDouble(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 3) {
        g_host->runtime_error(vm, "SqliteBindDouble expects (stmt_handle:int, index:int, value:real).");
        return g_host->make_int(-1);
    }
    sqlite3_stmt *stmt = NULL;
    if (!fetchStatement(vm, args, arg_count, &stmt)) {
        return g_host->make_int(-1);
    }
    if (!g_host->is_intlike_type(args[1].type)) {
        g_host->runtime_error(vm, "SqliteBindDouble index must be integer.");
        return g_host->make_int(-1);
    }
    int index = (int)g_host->as_int64(args[1]);
    if (index <= 0) {
        g_host->runtime_error(vm, "SqliteBindDouble parameter index must be >= 1.");
        return g_host->make_int(-1);
    }
    double value;
    if (g_host->is_real_type(args[2].type)) {
        value = g_host->as_double(args[2]);
    } else if (g_host->is_intlike_type(args[2].type)) {
        value = (double)g_host->as_int64(args[2]);
    } else {
        g_host->runtime_error(vm, "SqliteBindDouble value must be numeric.");
        return g_host->make_int(-1);
    }
    int rc = sqlite3_bind_double(stmt, index, value);
    if (rc != SQLITE_OK) {
        g_host->runtime_error(vm, "SqliteBindDouble failed (%d).", rc);
    }
    return g_host->make_int(rc);
}

static Value pluginSqliteBindNull(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 2) {
        g_host->runtime_error(vm, "SqliteBindNull expects (stmt_handle:int, index:int).");
        return g_host->make_int(-1);
    }
    sqlite3_stmt *stmt = NULL;
    if (!fetchStatement(vm, args, arg_count, &stmt)) {
        return g_host->make_int(-1);
    }
    if (!g_host->is_intlike_type(args[1].type)) {
        g_host->runtime_error(vm, "SqliteBindNull index must be integer.");
        return g_host->make_int(-1);
    }
    int index = (int)g_host->as_int64(args[1]);
    if (index <= 0) {
        g_host->runtime_error(vm, "SqliteBindNull parameter index must be >= 1.");
        return g_host->make_int(-1);
    }
    int rc = sqlite3_bind_null(stmt, index);
    if (rc != SQLITE_OK) {
        g_host->runtime_error(vm, "SqliteBindNull failed (%d).", rc);
    }
    return g_host->make_int(rc);
}

static Value pluginSqliteClearBindings(struct VM_s *vm, int arg_count, Value *args) {
    sqlite3_stmt *stmt = NULL;
    if (!fetchStatement(vm, args, arg_count, &stmt)) {
        return g_host->make_int(-1);
    }
    int rc = sqlite3_clear_bindings(stmt);
    if (rc != SQLITE_OK) {
        g_host->runtime_error(vm, "SqliteClearBindings failed (%d).", rc);
    }
    return g_host->make_int(rc);
}

static Value pluginSqliteErrMsg(struct VM_s *vm, int arg_count, Value *args) {
    sqlite3 *db = NULL;
    if (!fetchDatabase(vm, args, arg_count, &db)) {
        return g_host->make_string("");
    }
    const char *msg = sqlite3_errmsg(db);
    return g_host->make_string(msg ? msg : "");
}

static Value pluginSqliteLastInsertRowId(struct VM_s *vm, int arg_count, Value *args) {
    sqlite3 *db = NULL;
    if (!fetchDatabase(vm, args, arg_count, &db)) {
        return g_host->make_int64(0);
    }
    return g_host->make_int64((long long)sqlite3_last_insert_rowid(db));
}

static Value pluginSqliteChanges(struct VM_s *vm, int arg_count, Value *args) {
    sqlite3 *db = NULL;
    if (!fetchDatabase(vm, args, arg_count, &db)) {
        return g_host->make_int(0);
    }
    return g_host->make_int(sqlite3_changes(db));
}

static void registerOne(const char *group, const char *display_name, const char *vm_name,
                        PscalExtBuiltinFn fn) {
    g_host->register_function_entry("sqlite", group, display_name);
    g_host->register_builtin(vm_name, fn, PSCAL_EXT_BUILTIN_FUNCTION, display_name, PSCAL_EXT_FX_IO);
}

int pscal_ext_register(const PscalExtHostApi *host, uint32_t host_abi) {
    if (PSCAL_EXT_ABI_MAJOR_OF(host_abi) != PSCAL_EXT_ABI_MAJOR) {
        return 1;
    }
    g_host = host;
    g_handles = host->handle_table_create();
    if (!g_handles) {
        return 2;
    }

    const char *category = "sqlite";
    g_host->register_category(category);
    g_host->register_group(category, "connection");
    g_host->register_group(category, "statement");
    g_host->register_group(category, "metadata");
    g_host->register_group(category, "results");
    g_host->register_group(category, "binding");

    registerOne("connection", "SqliteOpen", "sqliteopen", pluginSqliteOpen);
    registerOne("connection", "SqliteClose", "sqliteclose", pluginSqliteClose);
    registerOne("connection", "SqliteExec", "sqliteexec", pluginSqliteExec);
    registerOne("statement", "SqlitePrepare", "sqliteprepare", pluginSqlitePrepare);
    registerOne("statement", "SqliteFinalize", "sqlitefinalize", pluginSqliteFinalize);
    registerOne("statement", "SqliteStep", "sqlitestep", pluginSqliteStep);
    registerOne("statement", "SqliteReset", "sqlitereset", pluginSqliteReset);
    registerOne("metadata", "SqliteColumnCount", "sqlitecolumncount", pluginSqliteColumnCount);
    registerOne("metadata", "SqliteColumnType", "sqlitecolumntype", pluginSqliteColumnType);
    registerOne("metadata", "SqliteColumnName", "sqlitecolumnname", pluginSqliteColumnName);
    registerOne("results", "SqliteColumnInt", "sqlitecolumnint", pluginSqliteColumnInt);
    registerOne("results", "SqliteColumnDouble", "sqlitecolumndouble", pluginSqliteColumnDouble);
    registerOne("results", "SqliteColumnText", "sqlitecolumntext", pluginSqliteColumnText);
    registerOne("binding", "SqliteBindText", "sqlitebindtext", pluginSqliteBindText);
    registerOne("binding", "SqliteBindInt", "sqlitebindint", pluginSqliteBindInt);
    registerOne("binding", "SqliteBindDouble", "sqlitebinddouble", pluginSqliteBindDouble);
    registerOne("binding", "SqliteBindNull", "sqlitebindnull", pluginSqliteBindNull);
    registerOne("statement", "SqliteClearBindings", "sqliteclearbindings", pluginSqliteClearBindings);
    registerOne("connection", "SqliteErrMsg", "sqliteerrmsg", pluginSqliteErrMsg);
    registerOne("connection", "SqliteLastInsertRowId", "sqlitelastinsertrowid", pluginSqliteLastInsertRowId);
    registerOne("connection", "SqliteChanges", "sqlitechanges", pluginSqliteChanges);

    return 0;
}
