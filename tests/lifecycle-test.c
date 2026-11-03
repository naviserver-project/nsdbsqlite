/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* Exercise callback states and finalization failures hidden by ns_db cleanup. */
#include "../sqlite3.h"
static int StatsStatus(sqlite3 *db, int operation, sqlite3_int64 *current,
                       sqlite3_int64 *peak, int reset);
#define sqlite3_db_status64 StatsStatus
#include "../nsdbsqlite.c"
#undef sqlite3_db_status64
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "failed: %s, line %d\n", #c, __LINE__);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static int statsUnsupported = -1;
static bool statsSynthetic = NS_FALSE;

/* Inject unsupported verbs without requiring a different SQLite build. */
static int
StatsStatus(sqlite3 *db, int operation, sqlite3_int64 *current, sqlite3_int64 *peak, int reset)
{
    if (statsUnsupported == -2 || statsUnsupported == operation) {
        return SQLITE_ERROR;
    }
    if (statsSynthetic && operation == SQLITE_DBSTATUS_LOOKASIDE_HIT) {
        *current = 0;
        *peak = 123;
        return SQLITE_OK;
    }
    if (statsSynthetic && operation == SQLITE_DBSTATUS_LOOKASIDE_USED) {
        *current = 2;
        *peak = 7;
        return SQLITE_OK;
    }
    return sqlite3_db_status64(db, operation, current, peak, reset);
}

static void StatsCheck(Ns_DbHandle *handle) NS_GNUC_NONNULL(1);

/* Verify omission and highwater semantics independently of build options. */
static void
StatsCheck(Ns_DbHandle *handle)
{
    Tcl_Interp *interp = Tcl_CreateInterp();

    statsUnsupported = SQLITE_DBSTATUS_CACHE_MISS;
    CHECK(DbStatsCollect(interp, handle, NULL, NS_FALSE) == TCL_OK);
    CHECK(Tcl_SetVar2Ex(interp, "stats", NULL, Tcl_GetObjResult(interp), 0) != NULL);
    CHECK(Tcl_Eval(interp, "expr {![dict exists $stats connection cacheMisses] && [dict exists $stats connection cacheHits]}") == TCL_OK);
    CHECK(strcmp(Tcl_GetStringResult(interp), "1") == 0);
    CHECK(DbStatsCollect(interp, handle, NULL, NS_TRUE) == TCL_OK);
    CHECK(Tcl_SetVar2Ex(interp, "stats", NULL, Tcl_GetObjResult(interp), 0) != NULL);
    CHECK(Tcl_Eval(interp, "expr {![dict get $stats connection cacheMisses available] && ![dict exists $stats connection cacheMisses current]}") == TCL_OK);
    CHECK(strcmp(Tcl_GetStringResult(interp), "1") == 0);

    statsUnsupported = -2;
    CHECK(DbStatsCollect(interp, handle, NULL, NS_FALSE) == TCL_OK);
    CHECK(Tcl_SetVar2Ex(interp, "stats", NULL, Tcl_GetObjResult(interp), 0) != NULL);
    CHECK(Tcl_Eval(interp, "expr {![dict exists $stats connection] && ![dict exists $stats backend sqlite connection] && ![dict exists $stats highwater]}") == TCL_OK);
    CHECK(strcmp(Tcl_GetStringResult(interp), "1") == 0);

    statsUnsupported = -1;
    statsSynthetic = NS_TRUE;
    CHECK(DbStatsCollect(interp, handle, NULL, NS_FALSE) == TCL_OK);
    CHECK(Tcl_SetVar2Ex(interp, "stats", NULL, Tcl_GetObjResult(interp), 0) != NULL);
    CHECK(Tcl_Eval(interp, "expr {[dict get $stats backend sqlite connection lookasideHits]==123 && [dict get $stats backend sqlite connection lookasideUsedSlots]==2 && [dict get $stats highwater backend sqlite connection lookasideUsedSlots]==7 && ![dict exists $stats highwater backend sqlite connection lookasideHits] && ![dict exists $stats highwater connection cacheUsedBytes]}") == TCL_OK);
    CHECK(strcmp(Tcl_GetStringResult(interp), "1") == 0);
    statsSynthetic = NS_FALSE;
    Tcl_DeleteInterp(interp);
}

/*
 *----------------------------------------------------------------------
 *
 * failedStep --
 *
 *      Leave a failed SQLite step pending to exercise cleanup callbacks.
 *
 * Results:
 *      None; aborts if the intended failure cannot be established.
 *
 * Side effects:
 *      Prepares a statement and records its failed SQLite step.
 *
 *----------------------------------------------------------------------
 */
static void
failedStep(Ns_DbHandle *handle)
{
    Context *contextPtr;
    char     sql[] = "SELECT abs(-9223372036854775808)";

    Tcl_DStringSetLength(&handle->dsExceptionMsg, 0);
    handle->cExceptionCode[0] = '\0';
    CHECK(DbExec(handle, sql) == NS_ROWS);
    contextPtr = handle->statement;
    CHECK(sqlite3_step(contextPtr->stmt) == SQLITE_ERROR);
}

/*
 *----------------------------------------------------------------------
 *
 * main --
 *
 *      Verify direct driver lifecycle callbacks and statement finalization.
 *
 * Results:
 *      Zero on success; exits on a failed check.
 *
 * Side effects:
 *      Creates an in-memory database and exercises successful and failed
 *      cleanup.
 *
 *----------------------------------------------------------------------
 */

int
main(int argc, char **argv)
{
    Ns_DbHandle handle = {0};
    Connection *connectionPtr;
    Context    *contextPtr;
    char        replacement[] = "CREATE TABLE must_not_execute (id INTEGER)";
    char        begin[] = "BEGIN";

    (void)argc;
    Tcl_FindExecutable(argv[0]);
    Nsd_LibInit();
    Tcl_DStringInit(&handle.dsExceptionMsg);
    handle.datasource = ":memory:";
    handle.row = Ns_SetCreate("test");
    CHECK(DbOpen(&handle) == NS_OK);
    StatsCheck(&handle);
    handle.fetchingRows = NS_TRUE;
    CHECK(DbCancel(&handle) == NS_OK && !handle.fetchingRows);
    contextPtr = ns_calloc(1, sizeof(Context));
    contextPtr->ncolumns = 1;
    handle.statement = contextPtr;
    handle.fetchingRows = NS_TRUE;
    CHECK(DbBindRow(&handle) == NULL);
    CHECK(DbGetRow(&handle, handle.row) == NS_ERROR);
    CHECK(DbCancel(&handle) == NS_OK);

    failedStep(&handle);
    CHECK(DbFlush(&handle) == NS_ERROR);
    CHECK(handle.statement == NULL && !handle.fetchingRows);
    CHECK(strstr(Tcl_DStringValue(&handle.dsExceptionMsg), "integer overflow") != NULL);
    CHECK(DbCancel(&handle) == NS_OK);

    failedStep(&handle);
    Ns_DbSetException(&handle, "NSDB", "primary diagnostic");
    CHECK(DbCancel(&handle) == NS_ERROR);
    CHECK(strcmp(Tcl_DStringValue(&handle.dsExceptionMsg), "primary diagnostic") == 0);

    failedStep(&handle);
    CHECK(DbExec(&handle, replacement) == NS_ERROR);
    connectionPtr = handle.connection;
    CHECK(sqlite3_exec(connectionPtr->db, "SELECT * FROM must_not_execute", NULL, NULL, NULL)
          != SQLITE_OK);

    CHECK(DbExec(&handle, begin) == NS_DML);
    failedStep(&handle);
    CHECK(DbResetHandle(&handle) == NS_ERROR);
    CHECK(sqlite3_get_autocommit(connectionPtr->db) != 0);
    CHECK(handle.statement == NULL && !handle.fetchingRows);

    failedStep(&handle);
    CHECK(DbClose(&handle) == NS_ERROR);
    CHECK(handle.connection == NULL && !handle.connected);
    Ns_SetFree(handle.row);
    Tcl_DStringFree(&handle.dsExceptionMsg);
    puts("Direct lifecycle/finalization checks passed.");
    return 0;
}
