/*
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://mozilla.org/.
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
 * the License for the specific language governing rights and limitations
 * under the License.
 *
 * The Original Code is AOLserver Code and related documentation
 * distributed by AOL.
 * 
 * The Initial Developer of the Original Code is America Online,
 * Inc. Portions created by AOL are Copyright (C) 1999 America Online,
 * Inc. All Rights Reserved.
 *
 * Alternatively, the contents of this file may be used under the terms
 * of the GNU General Public License (the "GPL"), in which case the
 * provisions of GPL are applicable instead of those above.  If you wish
 * to allow use of your version of this file only under the terms of the
 * GPL and not to allow others to use your version of this file under the
 * License, indicate your decision by deleting the provisions above and
 * replace them with the notice and other provisions required by the GPL.
 * If you do not delete the provisions above, a recipient may use your
 * version of this file under either the License or the GPL.
 */


/* 
 * nsdbsqlite.c --
 *
 *	This file implements the SQLite 3 database driver.
 */

static const char *RCSID = "@(#) $Header$, compiled: " __DATE__ " " __TIME__;

#include "ns.h"
#include "nsdb.h"
#include <sqlite3.h>

#define DRIVER_VERSION "0.9"


/*
 * Exported variables.
 */

NS_EXPORT int   Ns_ModuleVersion = 1;

typedef struct {
    long            ncolumns;
    char            **columns;
    long            nrows;
    char            ***rows;
    long            maxrows;
    long            row;
} Context;


/*
 * Local functions defined in this file.
 */

static Ns_Set *DbBindRow(Ns_DbHandle *handle);
static int DbCancel(Ns_DbHandle *handle);
static int DbClose(Ns_DbHandle *handle);
static int DbExec(Ns_DbHandle *handle, char *sql);
static int DbFlush(Ns_DbHandle *handle);
static int DbGetRow(Ns_DbHandle *handle, Ns_Set *row);
static int DbGetRowCount(Ns_DbHandle *handle);
static const char *DbName(void);
static int DbOpen(Ns_DbHandle *handle);
static int DbServerInit(char *server, char *module, char *driver);
static int DbSpExec(Ns_DbHandle *handle);
static int DbSpStart(Ns_DbHandle *handle, char *procname);
static const char *DbType(Ns_DbHandle *handle);
static int DbInterpInit(Tcl_Interp * interp, void *ignored);
static int DbExecCallback(void *arg, int ncolumns, char **row, char **columns);

static Tcl_ObjCmdProc DbCmd;

static Ns_DbProc dbProcs[] = {
    { DbFn_ServerInit,   DbServerInit },
    { DbFn_Name,         DbName },
    { DbFn_DbType,       DbType },
    { DbFn_OpenDb,       DbOpen },
    { DbFn_CloseDb,      DbClose },
    { DbFn_GetRow,       DbGetRow },
    { DbFn_GetRowCount,  DbGetRowCount },
    { DbFn_Flush,        DbFlush },
    { DbFn_Cancel,       DbCancel },
    { DbFn_Exec,         DbExec },
    { DbFn_BindRow,      DbBindRow },
    { DbFn_SpStart,      DbSpStart },
    { DbFn_SpExec,       DbSpExec },
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

NS_EXPORT int
Ns_DbDriverInit(char *driver, char *path)
{
    if (driver == NULL) {
        Ns_Log(Bug, "nsdbsqlite: Ns_DbDriverInit() called with NULL driver name.");
        return NS_ERROR;
    }
    if (Ns_DbRegisterDriver(driver, dbProcs) != NS_OK) {
        Ns_Log(Error, "nsdbsqlite: could not register the '%s' driver.", driver);
        return NS_ERROR;
    }
    Ns_Log(Notice, "nsdbsqlite: Loaded %s, version %s built on %s at %s.",
            driver, DRIVER_VERSION, __DATE__, __TIME__);
    return NS_OK;
}

static int DbInterpInit(Tcl_Interp * interp, void *ignored)
{
    Tcl_CreateObjCommand(interp, "ns_sqlite", DbCmd, NULL, NULL);
    return NS_OK;
}

static int DbServerInit(char *hServer, char *hModule, char *hDriver)
{
    Ns_TclRegisterTrace(hServer, DbInterpInit, NULL, NS_TCL_TRACE_CREATE);
    return NS_OK;
}

static const char *
DbName(void)
{
    return "nsdbsqlite";
}

static const char *
DbType(Ns_DbHandle *handle)
{
    return "sqlite";
}

static int
DbOpen(Ns_DbHandle *handle)
{
    sqlite3         *db = NULL;

    sqlite3_open(handle->datasource, &db);
    if (sqlite3_errcode(db) != SQLITE_OK) {
        Ns_Log(Error, "nsdbsqlite: couldn't open '%s': %s",
                handle->datasource, sqlite3_errmsg(db));
        Ns_DbSetException(handle, "NSDB", "couldn't open database");
        return NS_ERROR;
    }

    handle->connection = (void *) db;
    handle->connected = NS_TRUE;

    return NS_OK;
}

static int
DbClose(Ns_DbHandle *handle)
{
    sqlite3         *db = (sqlite3 *) handle->connection;

    sqlite3_close(db);
    handle->connected = NS_FALSE;

    return NS_OK;
}

static int
DbExec(Ns_DbHandle *handle, char *sql)
{
    sqlite3         *db = (sqlite3 *) handle->connection;
    Context         *contextPtr = NULL;
    int             status, rc;
    char            *err = NULL;

    if (handle->verbose) {
        Ns_Log(Notice, "nsdbsqlite: DbExec(%s), SQL = '%s'", handle->datasource, sql);
    }

    status = NS_OK;

    contextPtr = ns_calloc(1, sizeof(Context));
    contextPtr->ncolumns = 0;
    contextPtr->columns = NULL;
    contextPtr->nrows = 0;
    contextPtr->rows = NULL;
    contextPtr->maxrows = 0;
    contextPtr->row = 0;
    handle->statement = (void *) contextPtr;

    rc = sqlite3_exec(db, sql, DbExecCallback, (void *) handle, &err);
    if (err != NULL) {
        Ns_Log(Error, "nsdbsqlite: error executing SQL: %s", err);
        Ns_DbSetException(handle, "NSDB", "error executing SQL");
        sqlite3_free(err);
        status = NS_ERROR;
    } else
    if (rc != SQLITE_OK) {
        Ns_Log(Error, "nsdbsqlite: error executing SQL: %s", sqlite3_errmsg(db));
        Ns_DbSetException(handle, "NSDB", "error executing SQL");
        status = NS_ERROR;
    }

    if (status == NS_ERROR) {
        DbCancel(handle);
        return NS_ERROR;
    }

    if (contextPtr->nrows == 0) { 
        handle->fetchingRows = 0;
        status = NS_DML;
    } else {
        handle->fetchingRows = 1;
        status = NS_ROWS;
    }

    return status;
}

static int
DbExecCallback(void *arg, int ncolumns, char **row, char **columns)
{
    Ns_DbHandle     *handlePtr = (Ns_DbHandle *) arg;
    Context         *contextPtr = (Context *) handlePtr->statement;
    long            col;

    if (contextPtr->ncolumns == 0) {
        contextPtr->ncolumns = ncolumns;
        contextPtr->columns = ns_calloc((unsigned) ncolumns, sizeof(char *));
        for (col = 0; col < ncolumns; col++) {
            contextPtr->columns[col] = ns_strdup(columns[col]);
        }
    }

    if (contextPtr->nrows == contextPtr->maxrows) {
        if (contextPtr->maxrows == 0) {
            contextPtr->maxrows = 50;
            contextPtr->rows = ns_calloc((unsigned) contextPtr->maxrows, sizeof(char **));
        } else {
            contextPtr->maxrows *= 2;
            contextPtr->rows = ns_realloc(contextPtr->rows, contextPtr->maxrows * sizeof(char **));
        }
    }

    contextPtr->rows[contextPtr->nrows] = ns_calloc((unsigned) ncolumns, sizeof(char *));
    for (col = 0; col < ncolumns; col++) {
        contextPtr->rows[contextPtr->nrows][col] = ns_strdup(row[col]);
    }
    
    ++contextPtr->nrows;

    return 0;
}

static Ns_Set *
DbBindRow(Ns_DbHandle *handle)
{
    Context         *contextPtr = (Context *) handle->statement;
    Ns_Set          *row = (Ns_Set *) handle->row;
    long            col;

    if (contextPtr->columns == NULL) {
        Ns_DbSetException(handle, "NSDB", "no result data for row");
        return NULL;
    }

    for (col = 0; col < contextPtr->ncolumns; col++) {
        Ns_SetPut(row, contextPtr->columns[col], NULL);
    }

    return row;
}


static int
DbGetRow(Ns_DbHandle *handle, Ns_Set *row)
{
    Context         *contextPtr = (Context *) handle->statement;
    long            col;

    if (handle->statement == NULL || !handle->fetchingRows) {
        Ns_DbSetException(handle, "NSDB", "no rows waiting to fetch");
        return NS_ERROR;
    }

    if (contextPtr->ncolumns == 0) {
        DbCancel(handle);
        return NS_ERROR;
    }

    if (contextPtr->row >= contextPtr->nrows) {
        DbCancel(handle);
        return NS_END_DATA;
    }

    for (col = 0; col < contextPtr->ncolumns; col++) {
        Ns_SetPutValue(row, col, contextPtr->rows[contextPtr->row][col]);
    }

    ++contextPtr->row;

    return NS_OK;
}

static int
DbGetRowCount(Ns_DbHandle *handle, Ns_Set *row)
{
    Context         *contextPtr = (Context *) handle->statement;
    long            col;

    if (handle->statement == NULL || !handle->fetchingRows) {
        Ns_DbSetException(handle, "NSDB", "no rows waiting to fetch");
        return NS_ERROR;
    }
    return contextPtr->nrows;
}

static int
DbFlush(Ns_DbHandle *handle)
{
    return DbCancel(handle);
}

static int
DbCancel(Ns_DbHandle *handle)
{
    Context         *contextPtr = (Context *) handle->statement;
    int             col, row;

    if (handle->statement == NULL) {
        /* Already cancelled. */
        return NS_OK;
    }

    if (contextPtr->columns != NULL) {
        for (col = 0; col < contextPtr->ncolumns; col++) {
            ns_free(contextPtr->columns[col]);
        }
        ns_free(contextPtr->columns);
    }
    if (contextPtr->rows != NULL) {
        for (row = 0; row < contextPtr->nrows; row++) {
            for (col = 0; col < contextPtr->ncolumns; col++) {
                ns_free(contextPtr->rows[row][col]);
            }
            ns_free(contextPtr->rows[row]);
        }
        ns_free(contextPtr->rows);
    }
    ns_free(contextPtr);
    handle->statement = NULL;
    handle->fetchingRows = 0;

    return NS_OK;
}


static int
DbSpStart(Ns_DbHandle *handle, char *procname)
{
    return DbExec(handle, procname);
}

static int
DbSpExec(Ns_DbHandle *handle)
{
    Context         *contextPtr = (Context *) handle->statement;

    if (contextPtr == NULL) {
        return NS_ERROR;
    }

    return (contextPtr->nrows == 0 ? NS_DML : NS_ROWS);
}


/*
 *----------------------------------------------------------------------
 *
 * DbCmd --
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
DbCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj * CONST objv[])
{
    Ns_DbHandle         *handle;
    Context             *contextPtr;
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
    if (Tcl_GetIndexFromObj(interp, objv[1], opts, "option", 1,
                (int *) &opt) != TCL_OK) {
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
        /* == [ns_freetds rows_affected $db] == */
        contextPtr = (Context *) handle->statement;
        if (contextPtr == NULL) {
            Tcl_AppendResult(interp, "handle \"", Tcl_GetString(objv[2]),
                    "\" has no current statement", NULL);
            return TCL_ERROR;
        }
        Tcl_SetObjResult(interp, Tcl_NewLongObj(contextPtr->nrows));
        break;

    case IVersionIdx:
        /* == [ns_freetds version $db] == */
        Tcl_SetResult(interp, (char *) sqlite3_version, TCL_STATIC);
        break;
    }
    return TCL_OK;
}
