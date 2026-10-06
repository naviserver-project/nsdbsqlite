/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* Exercise callback states and finalization failures hidden by ns_db cleanup. */
#include "../nsdbsqlite.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "failed: %s, line %d\n", #c, __LINE__); exit(1); } } while (0)

static void
failedStep(Ns_DbHandle *handle)
{
    Context *contextPtr;
    char sql[] = "SELECT abs(-9223372036854775808)";

    Tcl_DStringSetLength(&handle->dsExceptionMsg, 0);
    handle->cExceptionCode[0] = '\0';
    CHECK(DbExec(handle, sql) == NS_ROWS);
    contextPtr = handle->statement;
    CHECK(sqlite3_step(contextPtr->stmt) == SQLITE_ERROR);
}

int
main(int argc, char **argv)
{
    Ns_DbHandle handle = {0};
    Connection *connectionPtr;
    Context *contextPtr;
    char replacement[] = "CREATE TABLE must_not_execute (id INTEGER)";
    char begin[] = "BEGIN";

    (void)argc;
    Tcl_FindExecutable(argv[0]);
    Nsd_LibInit();
    Tcl_DStringInit(&handle.dsExceptionMsg);
    handle.datasource = ":memory:";
    handle.row = Ns_SetCreate("test");
    CHECK(DbOpen(&handle) == NS_OK);
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
    CHECK(sqlite3_exec(connectionPtr->db, "SELECT * FROM must_not_execute", NULL, NULL, NULL) != SQLITE_OK);

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
