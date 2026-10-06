/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The Initial Developer of the Original Code and related documentation
 * is America Online, Inc. Portions created by AOL are Copyright (C) 1999
 * America Online, Inc. All Rights Reserved.
 */

/* 
 * nsdbsqlite.c --
 *
 *      Implements the nsdb driver interface for the  SQLite database.
 *
 *   Dossy Shiobara <dossy@panoptic.com>
 *   Author Vlad Seryakov <vlad@crystalballinc.com>
 *   Gustaf Neumann <neumann@wu.ac.at>
 *
 */

#include "ns.h"
#include "nsdb.h"
#include "sqlite3.h"
#include <limits.h>

#define DRIVER_VERSION "0.9"


/*
 * Exported variables.
 */

NS_EXPORT int   Ns_ModuleVersion = 1;
NS_EXPORT NsDb_DriverInitProc Ns_DbDriverInit;

#if defined(NS_MODULE_INFO_VERSION) && defined(NS_MODULE_TAG)
NS_EXPORT Ns_ModuleInfoProc Ns_ModuleGetInfo;
/* Provide module build and ABI information for runtime introspection. */
NS_EXPORT void
Ns_ModuleGetInfo(Ns_ModuleInfo *infoPtr)
{
    Ns_ModuleInfoInit(infoPtr, NS_MODULE_INFO_VERSION,
                      NS_MODULE_NAME,
                      DRIVER_VERSION,
                      NS_MODULE_TAG,
                      "db-driver",
                      1u);
}
#endif

typedef struct {
    sqlite3       *db;
    sqlite3_int64  nrows;
    sqlite3_int64  affected;
} Connection;

typedef struct {
    unsigned long   ncolumns;
    sqlite3_int64   totalBefore;
    sqlite3_stmt   *stmt;
    bool            spPending;
} Context;

/*
 * Local functions defined in this file.
 */

static Ns_Set *DbBindRow(Ns_DbHandle *handle);
static void DbError(Ns_DbHandle *handle, const char *operation);
static Ns_ReturnCode DbCancel(Ns_DbHandle *handle);
static int DbClose(Ns_DbHandle *handle);
static int DbExec(Ns_DbHandle *handle, char *sql);
static Ns_ReturnCode DbPrepare(Ns_DbHandle *handle, const char *sql);
static int DbExecutePrepared(Ns_DbHandle *handle);
static int DbFlush(Ns_DbHandle *handle);
static Ns_ReturnCode DbResetHandle(Ns_DbHandle *handle);
static int DbGetRow(Ns_DbHandle *handle, Ns_Set *row);
static int DbGetRowCount(Ns_DbHandle *handle);
static const char *DbName(void);
static Ns_ReturnCode DbOpen(Ns_DbHandle *handle);
static Ns_ReturnCode DbServerInit(char *server, char *module, char *driver);
static int DbSpExec(Ns_DbHandle *handle);
static Ns_ReturnCode DbSpStart(Ns_DbHandle *handle, char *procname);
static const char *DbType(Ns_DbHandle *handle);
#if NS_VERSION_NUM >= 50000
static Tcl_Obj *DbVersionInfo(Ns_DbHandle *handle);
#endif

static Ns_TclTraceProc DbInterpInit;
static TCL_OBJCMDPROC_T DbObjCmd;

static Ns_DbProc dbProcs[] = {
    { DbFn_ServerInit,   (ns_funcptr_t)DbServerInit },
    { DbFn_Name,         (ns_funcptr_t)DbName },
    { DbFn_DbType,       (ns_funcptr_t)DbType },
    { DbFn_OpenDb,       (ns_funcptr_t)DbOpen },
    { DbFn_CloseDb,      (ns_funcptr_t)DbClose },
    { DbFn_GetRow,       (ns_funcptr_t)DbGetRow },
    { DbFn_GetRowCount,  (ns_funcptr_t)DbGetRowCount },
    { DbFn_Flush,        (ns_funcptr_t)DbFlush },
    { DbFn_Cancel,       (ns_funcptr_t)DbCancel },
    { DbFn_ResetHandle,  (ns_funcptr_t)DbResetHandle },
    { DbFn_Exec,         (ns_funcptr_t)DbExec },
    { DbFn_BindRow,      (ns_funcptr_t)DbBindRow },
    { DbFn_SpStart,      (ns_funcptr_t)DbSpStart },
    { DbFn_SpExec,       (ns_funcptr_t)DbSpExec },
#if NS_VERSION_NUM >= 50000
    { DbFn_Version,     (ns_funcptr_t)DbVersionInfo },
#endif
    { 0, NULL }
};


/*
 *----------------------------------------------------------------------
 *
 * Ns_DbDriverInit --
 *
 *	Database driver module initialization routine.
 *
 * Results:
 *	NS_OK/NS_ERROR.
 *
 * Side effects:
 *	Database driver is registered.
 *
 *----------------------------------------------------------------------
 */

NS_EXPORT Ns_ReturnCode
Ns_DbDriverInit(const char *driver, const char *UNUSED(configPath))
{
    if (driver == NULL) {
        Ns_Log(Bug, "nsdbsqlite: Ns_DbDriverInit() called with NULL driver name.");
        return NS_ERROR;
    }
    if (Ns_DbRegisterDriver(driver, dbProcs) != NS_OK) {
        Ns_Log(Error, "nsdbsqlite: could not register the '%s' driver.", driver);
        return NS_ERROR;
    }
    Ns_Log(Notice, "nsdbsqlite: version %s loaded, based on SQLite %s (headers %s)",
           DRIVER_VERSION, sqlite3_libversion(), SQLITE_VERSION);
    return NS_OK;
}

static Ns_ReturnCode DbInterpInit(Tcl_Interp * interp, const void *UNUSED(arg))
{
    TCL_CREATEOBJCOMMAND(interp, "ns_sqlite", DbObjCmd, NULL, NULL);
    return NS_OK;
}

static Ns_ReturnCode DbServerInit(char *server, char *UNUSED(module), char *UNUSED(driver))
{
    Ns_TclRegisterTrace(server, DbInterpInit, NULL, NS_TCL_TRACE_CREATE);
    return NS_OK;
}

static const char *
DbName(void)
{
    return "sqlite";
}

static const char *
DbType(Ns_DbHandle *UNUSED(handle))
{
    return "sqlite";
}

#if NS_VERSION_NUM >= 50000
/* SQLite's client library and database engine are the same runtime library. */
static Tcl_Obj *
DbVersionInfo(Ns_DbHandle *UNUSED(handle))
{
    Tcl_Obj *dictObj = Tcl_NewDictObj();
    int version = sqlite3_libversion_number();

    Tcl_DictObjPut(NULL, dictObj,
                   Tcl_NewStringObj("clientversion", 13),
                   Tcl_NewIntObj(version));
    Tcl_DictObjPut(NULL, dictObj,
                   Tcl_NewStringObj("serverversion", 13),
                   Tcl_NewIntObj(version));
    return dictObj;
}
#endif

/* Preserve the SQLite diagnostic before finalizing a statement or connection. */
static void
DbError(Ns_DbHandle *handle, const char *operation)
{
    const char *message = sqlite3_errmsg(((Connection *)handle->connection)->db);

    Ns_Log(Error, "nsdbsqlite: %s: %s", operation, message);
    Ns_DbSetException(handle, "NSDB", message);
}

static Ns_ReturnCode
DbOpen(Ns_DbHandle *handle)
{
    sqlite3         *db = NULL;
    Connection      *connectionPtr;

    int rc = sqlite3_open(handle->datasource, &db);

    if (rc != SQLITE_OK) {
        const char *message = db != NULL ? sqlite3_errmsg(db) : sqlite3_errstr(rc);

        Ns_Log(Error, "nsdbsqlite: couldn't open '%s': %s", handle->datasource, message);
        Ns_DbSetException(handle, "NSDB", message);
        if (db != NULL) {
            sqlite3_close(db);
        }
        return NS_ERROR;
    }

    connectionPtr = ns_calloc(1, sizeof(Connection));

    connectionPtr->db = db;
    handle->connection = connectionPtr;
    handle->connected = NS_TRUE;
    handle->statement = NULL;

    return NS_OK;
}

static int
DbClose(Ns_DbHandle *handle)
{
    Connection      *connectionPtr = (Connection *)handle->connection;
    sqlite3         *db = connectionPtr->db;

    int status = DbCancel(handle);

    if (sqlite3_close(db) != SQLITE_OK) {
        DbError(handle, "closing database");
        return NS_ERROR;
    }
    ns_free(connectionPtr);
    handle->connection = NULL;
    handle->connected = NS_FALSE;

    return status;
}

/* Shared preparation keeps ordinary execution and the sp_* shim in sync. */
static Ns_ReturnCode
DbPrepare(Ns_DbHandle *handle, const char *sql)
{
    Connection *connectionPtr = (Connection *)handle->connection;
    Context *contextPtr;
    int rc;

    if (DbCancel(handle) != NS_OK) {
        return NS_ERROR;
    }
    contextPtr = ns_calloc(1, sizeof(Context));
    connectionPtr->nrows = 0;
    connectionPtr->affected = 0;
    contextPtr->totalBefore = sqlite3_total_changes64(connectionPtr->db);
    handle->statement = contextPtr;

    rc = sqlite3_prepare_v2(connectionPtr->db, sql, -1, &contextPtr->stmt, NULL);
    if (rc != SQLITE_OK) {
        DbError(handle, "preparing SQL");
        DbCancel(handle);
        return NS_ERROR;
    }
    contextPtr->ncolumns = (unsigned long)sqlite3_column_count(contextPtr->stmt);
    return NS_OK;
}

static int
DbExecutePrepared(Ns_DbHandle *handle)
{
    Connection *connectionPtr = (Connection *)handle->connection;
    sqlite3 *db = connectionPtr->db;
    Context *contextPtr = (Context *)handle->statement;

    if (contextPtr->ncolumns == 0) {
        handle->fetchingRows = NS_FALSE;
        if (sqlite3_step(contextPtr->stmt) != SQLITE_DONE) {
            DbError(handle, "executing SQL");
            DbCancel(handle);
            return NS_ERROR;
        }
        /* DDL and transaction commands leave sqlite3_changes unchanged. */
        if (sqlite3_total_changes64(db) != contextPtr->totalBefore) {
            connectionPtr->affected = sqlite3_changes64(db);
        }
        connectionPtr->nrows = connectionPtr->affected;
        return NS_DML;
    }
    handle->fetchingRows = NS_TRUE;
    return NS_ROWS;
}

static int
DbExec(Ns_DbHandle *handle, char *sql)
{
    if (DbPrepare(handle, sql) != NS_OK) {
        return NS_ERROR;
    }
    return DbExecutePrepared(handle);
}

static Ns_Set *
DbBindRow(Ns_DbHandle *handle)
{
    Context         *contextPtr = (Context *) handle->statement;
    Ns_Set          *row = (Ns_Set *) handle->row;
    unsigned long    col;

    if (contextPtr == NULL || contextPtr->stmt == NULL
        || !handle->fetchingRows || contextPtr->ncolumns == 0) {
        Ns_DbSetException(handle, "NSDB", "no result data for row");
        return NULL;
    }

    for (col = 0; col < contextPtr->ncolumns; col++) {
      Ns_SetPut(row, sqlite3_column_name(contextPtr->stmt, (int)col), NULL);
    }

    return row;
}


static int
DbGetRow(Ns_DbHandle *handle, Ns_Set *row)
{
    Context         *contextPtr = (Context *) handle->statement;
    unsigned long   col;
    int             status;

    if (contextPtr == NULL || contextPtr->stmt == NULL || !handle->fetchingRows) {
        Ns_DbSetException(handle, "NSDB", "no rows waiting to fetch");
        return NS_ERROR;
    }

    if (contextPtr->ncolumns == 0) {
        Ns_DbSetException(handle, "NSDB", "no result data for row");
        DbCancel(handle);
        return NS_ERROR;
    }
    if (Ns_SetSize(row) != contextPtr->ncolumns) {
        Ns_DbSetException(handle, "NSDB", "row set does not match result columns");
        return NS_ERROR;
    }

    if ((status = sqlite3_step(contextPtr->stmt)) == SQLITE_DONE) {
        Connection *connectionPtr = (Connection *)handle->connection;

        /* RETURNING statements publish their affected count at completion. */
        if (sqlite3_total_changes64(connectionPtr->db) != contextPtr->totalBefore) {
            connectionPtr->affected = sqlite3_changes64(connectionPtr->db);
        }
        if (DbCancel(handle) != NS_OK) {
            return NS_ERROR;
        }
        return NS_END_DATA;
    }

    if (status != SQLITE_ROW) {
        DbError(handle, "fetching row");
        DbCancel(handle);
        return NS_ERROR;
    }

    ((Connection *)handle->connection)->nrows++;
    for (col = 0; col < contextPtr->ncolumns; col++) {
      Ns_SetPutValue(row, col, (const char *)sqlite3_column_text(contextPtr->stmt, (int)col));
    }

    return NS_OK;
}

static int
DbGetRowCount(Ns_DbHandle *handle)
{
    const Connection *connectionPtr = (Connection *)handle->connection;

    if (connectionPtr->nrows > INT_MAX) {
        Ns_DbSetException(handle, "NSDB", "row count exceeds ns_db integer range");
        return NS_ERROR;
    }
    return (int)connectionPtr->nrows;
}

static int
DbFlush(Ns_DbHandle *handle)
{
    return DbCancel(handle);
}

/* Reset both explicit calls and connections being returned to the pool. */
static Ns_ReturnCode
DbResetHandle(Ns_DbHandle *handle)
{
    Connection *connectionPtr = (Connection *)handle->connection;
    sqlite3 *db = connectionPtr->db;

    Ns_ReturnCode status = DbCancel(handle);

    connectionPtr->nrows = 0;
    connectionPtr->affected = 0;
    /* SQLite tracks BEGIN and outermost SAVEPOINT transactions alike. */
    if (sqlite3_get_autocommit(db) == 0) {
        if (sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL) != SQLITE_OK) {
            DbError(handle, "rolling back unfinished transaction");
            return NS_ERROR;
        }
    }
    return status;
}


static Ns_ReturnCode
DbCancel(Ns_DbHandle *handle)
{
    Context *contextPtr = (Context *)handle->statement;
    int rc = SQLITE_OK;

    /* Detach the context even when finalization reports an execution error. */
    handle->statement = NULL;
    handle->fetchingRows = NS_FALSE;
    if (contextPtr != NULL) {
        rc = sqlite3_finalize(contextPtr->stmt);
        ns_free(contextPtr);
    }
    if (rc != SQLITE_OK) {
        /* Keep the primary prepare/step diagnostic if already recorded. */
        if (Tcl_DStringLength(&handle->dsExceptionMsg) == 0) {
            DbError(handle, "finalizing SQL");
        }
        return NS_ERROR;
    }
    return NS_OK;
}


/* SQLite has no native stored procedures; procname is SQL for this shim. */
static Ns_ReturnCode
DbSpStart(Ns_DbHandle *handle, char *procname)
{
    Context *contextPtr;

    if (DbPrepare(handle, procname) != NS_OK) {
        return NS_ERROR;
    }
    contextPtr = (Context *)handle->statement;
    if (contextPtr->stmt == NULL) {
        Ns_DbSetException(handle, "NSDB", "no SQL statement for sp_start");
        DbCancel(handle);
        return NS_ERROR;
    }
    if (sqlite3_bind_parameter_count(contextPtr->stmt) != 0) {
        Ns_DbSetException(handle, "NSDB", "sp_start does not support SQL parameters");
        DbCancel(handle);
        return NS_ERROR;
    }
    contextPtr->spPending = NS_TRUE;
    return NS_OK;
}

static int
DbSpExec(Ns_DbHandle *handle)
{
    Context *contextPtr = (Context *)handle->statement;

    if (contextPtr == NULL || contextPtr->stmt == NULL || !contextPtr->spPending) {
        Ns_DbSetException(handle, "NSDB", "no prepared SQL statement waiting for sp_exec");
        return NS_ERROR;
    }
    /* Consume the pending execution even when stepping fails. */
    contextPtr->spPending = NS_FALSE;
    return DbExecutePrepared(handle);
}


/*
 *----------------------------------------------------------------------
 *
 * DbObjCmd --
 *
 *	Implement the ns_sqlite command.
 *
 * Results:
 *	Standard Tcl result.
 *
 * Side effects:
 *	Depends on command.
 *
 *----------------------------------------------------------------------
 */


static int
DbObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj * CONST objv[])
{
    Ns_DbHandle         *handle;
    static CONST char   *opts[] = {
        "rows_affected", "version", NULL
    };
    enum {
        IRowsAffectedIdx, IVersionIdx
    } opt;

    if (objc < 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "option handle ?args?");
        return TCL_ERROR;
    }

    if (Tcl_GetIndexFromObj(interp, objv[1], opts, "option", 1, (int *) &opt) != TCL_OK) {
        return TCL_ERROR;
    }

    if (Ns_TclDbGetHandle(interp, Tcl_GetString(objv[2]), &handle) != TCL_OK) {
        return TCL_ERROR;
    }

    if (!STREQ(Ns_DbDriverName(handle), DbName())) {
        Tcl_AppendResult(interp, "handle \"", Tcl_GetString(objv[2]),
                "\" is not of type \"", DbName(), "\"", NULL);
        return TCL_ERROR;
    }

    switch (opt) {
    case IRowsAffectedIdx:
        Tcl_SetObjResult(interp, Tcl_NewWideIntObj(
                (Tcl_WideInt)((Connection *)handle->connection)->affected));
        break;

    case IVersionIdx:
        /* == [ns_freetds version $db] == */
        Tcl_SetResult(interp, (char *) sqlite3_version, TCL_STATIC);
        break;
    }
    return TCL_OK;
}

