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

/* String literals only; concatenation prevents accidental pointer arguments. */
#define NEW_LITERAL_OBJ(literal) Tcl_NewStringObj("" literal, sizeof("" literal) - 1)


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

static Ns_ReturnCode DbConfigureDatasources(const char *driver);
static Ns_Set *DbBindRow(Ns_DbHandle *handle);
static void DbError(Ns_DbHandle *handle, const char *operation);
static Ns_ReturnCode DbCancel(Ns_DbHandle *handle);
static int DbClose(Ns_DbHandle *handle);
static int DbExec(Ns_DbHandle *handle, char *sql);
static Ns_ReturnCode DbPrepare(Ns_DbHandle *handle, const char *sql);
static bool DbSqlTailIsEmpty(const char *tail);
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

#ifdef NS_DB_BATCH_ABI
static Ns_DbBatchDriverProc DbBatchOps;
static Tcl_Obj *BatchCapabilities(void);

static bool                  BatchSupports(const Ns_DbBatchColumn *column) NS_GNUC_NONNULL(1);
static Ns_DbBatchTransaction BatchTransaction(Ns_DbHandle *handle) NS_GNUC_NONNULL(1);
static int  BatchFail(Tcl_Interp *interp, Ns_DbHandle *handle, const char *code, Tcl_WideInt row,
                      const char *column, const char *message) NS_GNUC_NONNULL(1, 2, 3, 6);

static bool BatchInteger(const unsigned char *bytes, size_t length, bool micros,
                         sqlite3_int64 *value) NS_GNUC_NONNULL(1, 4);

static bool BatchTimestamp(const unsigned char *bytes, size_t length, sqlite3_int64 *value)
    NS_GNUC_NONNULL(1, 3);
static void BatchQuote(Tcl_DString *sql, const char *name) NS_GNUC_NONNULL(1, 2);
static int  BatchAffinity(const char *type) NS_GNUC_NONNULL(1);
static int  BatchInsert(Tcl_Interp *interp, Ns_DbHandle *handle, const Ns_DbBatch *batch,
                        Tcl_Obj *table, const Ns_DbBatchMapping *mapping, size_t mapped,
                        Tcl_WideInt *inserted) NS_GNUC_NONNULL(1, 2, 3, 4, 5, 7);

static const Ns_DbBatchDriverOps batchOps = {NS_DB_BATCH_ABI,     sizeof(Ns_DbBatchDriverOps),
                                             "prepared-bindings", BatchSupports,
                                             BatchCapabilities,   BatchTransaction,
                                             BatchInsert};
#endif

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
#ifdef NS_DB_BATCH_ABI
    { DbFn_Batch,       (ns_funcptr_t)DbBatchOps },
#endif
    { 0, NULL }
};

#ifdef NS_DB_BATCH_ABI
/* Optional native batch sink. No commits, fallback SQL text, or Tcl row values. */

/*
 *----------------------------------------------------------------------
 *
 * BatchSupports --
 *
 *      Check support for a JSON base type and its schema refinements.
 *
 * Results:
 *      True for descriptors handled by this SQLite sink.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static bool
BatchSupports(const Ns_DbBatchColumn *c)
{
    const char *r = c->refinement, *p = c->representation;

    if (strcmp(c->type, "null") == 0) {
        return r == NULL && p == NULL;
    }
    if (strcmp(c->type, "boolean") == 0) {
        return r == NULL && p == NULL;
    }
    if (strcmp(c->type, "string") == 0) {
        if (r != NULL && strcmp(r, "encoded_binary") == 0) {
            return p != NULL && strcmp(p, "hex") == 0;
        }
        if (r != NULL && strcmp(r, "accesslog_time") == 0) {
            return p != NULL && strcmp(p, "nslog_timestamp") == 0;
        }
        return r == NULL
               && (p == NULL || strcmp(p, "decoded_utf8") == 0 || strcmp(p, "nslog_timestamp") == 0
                   || strcmp(p, "nslog_fields") == 0);
    }
    if (strcmp(c->type, "number") != 0) {
        return false;
    }
    if (r == NULL) {
        return p == NULL || strcmp(p, "integer_decimal") == 0;
    }
    if (strcmp(r, "int32") == 0 || strcmp(r, "int64") == 0) {
        return p == NULL || strcmp(p, "integer_decimal") == 0;
    }
    if (strcmp(r, "duration_us") == 0) {
        return p != NULL && strcmp(p, "seconds_decimal") == 0;
    }
    if (strcmp(r, "datetime_us") == 0) {
        return p != NULL && strcmp(p, "unix_seconds_decimal") == 0;
    }
    return false;
}

/*
 *----------------------------------------------------------------------
 *
 * BatchCapabilities --
 *
 *      Describe native SQLite batch conversion and insertion capabilities.
 *
 * Results:
 *      New capability dictionary using ordinary Tcl strings.
 *
 * Side effects:
 *      Allocates the dictionary, keys and description strings.
 *
 *----------------------------------------------------------------------
 */
static Tcl_Obj *
BatchCapabilities(void)
{
    Tcl_Obj *d = Tcl_NewDictObj();

    (void)Tcl_DictObjPut(NULL, d,
                         NEW_LITERAL_OBJ("baseTypes"),
                         NEW_LITERAL_OBJ("string number boolean null"));
    (void)Tcl_DictObjPut(NULL, d,
                         NEW_LITERAL_OBJ("refinements"),
                         NEW_LITERAL_OBJ("int32 int64 duration_us datetime_us accesslog_time encoded_binary"));
    (void)Tcl_DictObjPut(NULL, d,
                         NEW_LITERAL_OBJ("representations"),
                         NEW_LITERAL_OBJ("compact-schema-lexical"));
    (void)Tcl_DictObjPut(NULL, d,
                         NEW_LITERAL_OBJ("conversion"),
                         NEW_LITERAL_OBJ("checked-int64 exact-microseconds utf8-text hex-blob"));
    (void)Tcl_DictObjPut(NULL, d,
                         NEW_LITERAL_OBJ("affinities"),
                         NEW_LITERAL_OBJ("validated-before-insertion"));
    return d;
}

/*
 *----------------------------------------------------------------------
 *
 * BatchTransaction --
 *
 *      Determine whether the handle has an active SQLite transaction.
 *
 * Results:
 *      Batch transaction state.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static Ns_DbBatchTransaction
BatchTransaction(Ns_DbHandle *h)
{
    Connection *c = h->connection;

    if (!h->connected || c == NULL || c->db == NULL) {
        return NS_DB_BATCH_UNKNOWN;
    }
    return sqlite3_get_autocommit(c->db) ? NS_DB_BATCH_IDLE : NS_DB_BATCH_ACTIVE;
}

/*
 *----------------------------------------------------------------------
 *
 * BatchFail --
 *
 *      Report a sink error with row and column context.
 *
 * Results:
 *      TCL_ERROR.
 *
 * Side effects:
 *      Sets the database exception, Tcl result and structured error code.
 *
 *----------------------------------------------------------------------
 */
static int
BatchFail(Tcl_Interp *interp, Ns_DbHandle *h, const char *code, Tcl_WideInt row, const char *column,
          const char *message)
{
    char     position[32];
    Tcl_Obj *error = Tcl_ObjPrintf("batch row %" TCL_LL_MODIFIER "d field %s: %s", row,
                                   column == NULL ? "<schema>" : column, message);

    Ns_DbSetException(h, "NSDB", Tcl_GetString(error));
    Tcl_SetObjResult(interp, error);
    snprintf(position, sizeof(position), "%" TCL_LL_MODIFIER "d", row);
    Tcl_SetErrorCode(interp, "NS_DB", "BATCH", code, position, column == NULL ? "" : column, NULL);
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * BatchInteger --
 *
 *      Convert a lexical integer or decimal seconds to a checked int64.
 *      Use integer arithmetic without floating point, leaving the source
 *      byte sequence unchanged.
 *
 * Results:
 *      True on exact conversion, false on syntax, precision or range failure.
 *
 * Side effects:
 *      Writes the converted value on success.
 *
 *----------------------------------------------------------------------
 */
static bool
BatchInteger(const unsigned char *s, size_t n, bool micros, sqlite3_int64 *out)
{
    size_t   i = 0, digits = 0, places = 0;
    bool     negative = false;
    uint64_t value = 0, limit, fraction = 0;

    if (n == 0u) {
        return false;
    }
    if (s[i] == '-') {
        negative = true;
        i++;
    }
    if (i + 1u < n && s[i] == '0' && s[i + 1u] >= '0' && s[i + 1u] <= '9') {
        return false;
    }
    limit = (uint64_t)INT64_MAX + (negative ? 1u : 0u);
    while (i < n && s[i] >= '0' && s[i] <= '9') {
        unsigned d = s[i++] - '0';
        if (value > (limit - d) / 10u) {
            return false;
        }
        value = value * 10u + d;
        digits++;
    }
    if (!digits) {
        return false;
    }
    if (micros) {
        if (value > limit / 1000000u) {
            return false;
        }
        value *= 1000000u;
        if (i < n && s[i] == '.') {
            i++;
            while (i < n && s[i] >= '0' && s[i] <= '9') {
                unsigned d = s[i++] - '0';
                if (places < 6u) {
                    fraction = fraction * 10u + d;
                } else if (d != 0u) {
                    return false;
                }
                places++;
            }
            if (places == 0u) {
                return false;
            }
            while (places++ < 6u) {
                fraction *= 10u;
            }
        }
        if (value > limit - fraction) {
            return false;
        }
        value += fraction;
    }
    if (i != n) {
        return false;
    }
    *out = negative ? (value == (uint64_t)INT64_MAX + 1u ? INT64_MIN : -(sqlite3_int64)value)
                    : (sqlite3_int64)value;
    return true;
}

/*
 *----------------------------------------------------------------------
 *
 * BatchTimestamp --
 *
 *      Convert an access-log calendar timestamp to UTC microseconds,
 *      independent of locale and timezone settings. Gregorian arithmetic
 *      remains integral; the original lexical value in the batch is unchanged.
 *
 * Results:
 *      True for a valid, representable timestamp.
 *
 * Side effects:
 *      Writes the converted value on success using integer arithmetic.
 *
 *----------------------------------------------------------------------
 */
static bool
BatchTimestamp(const unsigned char *s, size_t n, sqlite3_int64 *out)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    static const int  monthDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int               year = 0, day, month = -1, hour, minute, second, zoneHour, zoneMinute;
    int64_t           days;
    bool              leap;

    if (BatchInteger(s, n, true, out)) {
        return true;
    }
    if (n != 26u || s[2] != '/' || s[6] != '/' || s[11] != ':' || s[14] != ':' || s[17] != ':'
        || s[20] != ' ' || (s[21] != '+' && s[21] != '-')) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (i == 2u || (i >= 3u && i <= 6u) || i == 11u || i == 14u || i == 17u || i == 20u
            || i == 21u) {
            continue;
        }
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }
    for (int m = 0; m < 12; m++) {
        if (memcmp(s + 3, months + m * 3, 3) == 0) {
            month = m;
            break;
        }
    }
    for (size_t i = 7; i < 11; i++) {
        year = year * 10 + s[i] - '0';
    }
    day = (s[0] - '0') * 10 + s[1] - '0';
    hour = (s[12] - '0') * 10 + s[13] - '0';
    minute = (s[15] - '0') * 10 + s[16] - '0';
    second = (s[18] - '0') * 10 + s[19] - '0';
    zoneHour = (s[22] - '0') * 10 + s[23] - '0';
    zoneMinute = (s[24] - '0') * 10 + s[25] - '0';
    leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (year < 1 || month < 0 || day < 1 || day > monthDays[month] + (month == 1 && leap ? 1 : 0)
        || hour > 23 || minute > 59 || second > 59 || zoneHour > 23 || zoneMinute > 59) {
        return false;
    }
    days = (int64_t)365 * (year - 1970) + (year - 1) / 4 - 1969 / 4 - (year - 1) / 100 + 1969 / 100
           + (year - 1) / 400 - 1969 / 400;
    for (int m = 0; m < month; m++) {
        days += monthDays[m] + (m == 1 && leap ? 1 : 0);
    }
    days += day - 1;
    *out = (days * 86400 + hour * 3600 + minute * 60 + second
            - (s[21] == '+' ? 1 : -1) * (zoneHour * 3600 + zoneMinute * 60))
           * 1000000;
    return true;
}

/*
 *----------------------------------------------------------------------
 *
 * BatchQuote --
 *
 *      Append one safely quoted SQLite identifier.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Appends quoted identifier bytes to the SQL buffer.
 *
 *----------------------------------------------------------------------
 */
static void
BatchQuote(Tcl_DString *sql, const char *name)
{
    Tcl_DStringAppend(sql, "\"", 1);
    while (*name != '\0') {
        if (*name == '"') {
            Tcl_DStringAppend(sql, "\"", 1);
        }
        Tcl_DStringAppend(sql, name++, 1);
    }
    Tcl_DStringAppend(sql, "\"", 1);
}

enum { B_AFF_INTEGER, B_AFF_TEXT, B_AFF_BLOB, B_AFF_REAL, B_AFF_NUMERIC };

/*
 *----------------------------------------------------------------------
 *
 * BatchAffinity --
 *
 *      Classify a declared SQLite column type by affinity.
 *
 * Results:
 *      SQLite affinity category.
 *
 * Side effects:
 *      Allocates and releases a temporary uppercase type string.
 *
 *----------------------------------------------------------------------
 */
static int
BatchAffinity(const char *type)
{
    Tcl_DString upper;
    int         result = B_AFF_NUMERIC;

    Tcl_DStringInit(&upper);
    Tcl_DStringAppend(&upper, type, -1);
    Ns_StrToUpper(upper.string);
    if (strstr(upper.string, "INT") != NULL) {
        result = B_AFF_INTEGER;
    } else if (strstr(upper.string, "CHAR") != NULL || strstr(upper.string, "CLOB") != NULL
               || strstr(upper.string, "TEXT") != NULL) {
        result = B_AFF_TEXT;
    } else if (*type == '\0' || strstr(upper.string, "BLOB") != NULL) {
        result = B_AFF_BLOB;
    } else if (strstr(upper.string, "REAL") != NULL || strstr(upper.string, "FLOA") != NULL
               || strstr(upper.string, "DOUB") != NULL) {
        result = B_AFF_REAL;
    }
    Tcl_DStringFree(&upper);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * BatchHex --
 *
 *      Decode one hexadecimal digit.
 *
 * Results:
 *      Digit value, or a negative value for invalid input.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
BatchHex(unsigned char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/*
 *----------------------------------------------------------------------
 *
 * BatchInsert --
 *
 *      Insert lexical batch rows using validated native SQLite bindings.
 *
 * Results:
 *      TCL_OK with the inserted count, or TCL_ERROR.
 *
 * Side effects:
 *      Writes in the caller transaction and finalizes statements. Earlier
 *      rows remain pending on error; the caller must roll back.
 *
 *----------------------------------------------------------------------
 */
static int
BatchInsert(Tcl_Interp *interp, Ns_DbHandle *handle, const Ns_DbBatch *batch, Tcl_Obj *table,
            const Ns_DbBatchMapping *map, size_t mapped, Tcl_WideInt *inserted)
{
    Connection *connection = handle->connection;

    sqlite3      *db = connection->db;
    sqlite3_stmt *stmt = NULL;
    Tcl_Obj     **parts;
    TCL_SIZE_T    nParts;
    Tcl_DString   sql, utf;
    Tcl_Encoding  encoding = Tcl_GetEncoding(interp, "utf-8");
    int           result = TCL_ERROR, rc, step = SQLITE_DONE, *affinity = ns_calloc(mapped, sizeof(int));
    bool         *found = ns_calloc(mapped, sizeof(bool));
    const char   *database = "main", *tableName, *column = NULL;
    Tcl_WideInt  current = -1;

    Tcl_DStringInit(&sql);
    Tcl_DStringInit(&utf);
    *inserted = 0;
    
    if (handle->statement != NULL) {
        Context *context = handle->statement;

        if (context->spPending || context->ncolumns != 0u) {
            BatchFail(interp, handle, "INSERT", -1, NULL, "conflicting prepared/result operation");
            goto finish;
        }
        if (DbCancel(handle) != NS_OK) {
            BatchFail(interp, handle, "INSERT", -1, NULL,
                      Tcl_DStringValue(&handle->dsExceptionMsg));
            goto finish;
        }
    }
    Tcl_DStringSetLength(&handle->dsExceptionMsg, 0);
    handle->cExceptionCode[0] = '\0';
    if (Tcl_ListObjGetElements(interp, table, &nParts, &parts) != TCL_OK || nParts < 1
        || nParts > 2) {
        goto finish;
    }
    
    tableName = Tcl_GetString(parts[nParts - 1]);
    if (nParts == 2) {
        database = Tcl_GetString(parts[0]);
    }
    if (*database == '\0' || *tableName == '\0') {
        BatchFail(interp, handle, "MAPPING", -1, NULL, "empty table identifier");
        goto finish;
    }

    /*
     * Resolve SQLite's temp-before-main rule for unqualified table names.
     */
    for (int attempt = nParts == 1 ? 0 : 1; attempt < 2; attempt++) {
        const char *schema = nParts == 1 && attempt == 0 ? "temp" : database;

        Tcl_DStringSetLength(&sql, 0);
        Tcl_DStringAppend(&sql, "PRAGMA ", -1);
        BatchQuote(&sql, schema);
        Tcl_DStringAppend(&sql, ".table_xinfo(", -1);
        BatchQuote(&sql, tableName);
        Tcl_DStringAppend(&sql, ")", 1);
        Tcl_UtfToExternalDString(encoding, sql.string, sql.length, &utf);
        rc = sqlite3_prepare_v2(db, utf.string, utf.length, &stmt, NULL);
        Tcl_DStringFree(&utf);
        Tcl_DStringInit(&utf);
        if (rc != SQLITE_OK) {
            BatchFail(interp, handle, "MAPPING", -1, NULL, sqlite3_errmsg(db));
            goto finish;
        }
        step = sqlite3_step(stmt);
        if (step == SQLITE_ROW) {
            database = schema;
            break;
        }
        if (step != SQLITE_DONE) {
            BatchFail(interp, handle, "MAPPING", -1, NULL, sqlite3_errmsg(db));
            goto finish;
        }
        sqlite3_finalize(stmt);
        stmt = NULL;
    }
    if (stmt == NULL) {
        BatchFail(interp, handle, "MAPPING", -1, NULL, "target table does not exist");
        goto finish;
    }
    while (step == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(stmt, 1),
                   *decl = (const char *)sqlite3_column_text(stmt, 2);

        for (size_t c = 0; c < mapped; c++) {
            Tcl_DString nameUtf;

            Tcl_ExternalToUtfDString(encoding, name, -1, &nameUtf);
            if (sqlite3_stricmp(map[c].destination, nameUtf.string) == 0) {
                if (sqlite3_column_int(stmt, 6) != 0) {
                    Tcl_DStringFree(&nameUtf);
                    BatchFail(interp, handle, "MAPPING", -1, map[c].destination,
                              "generated or hidden target column");
                    goto finish;
                }
                found[c] = true;
                affinity[c] = BatchAffinity(decl == NULL ? "" : decl);
            }
            Tcl_DStringFree(&nameUtf);
        }
        step = sqlite3_step(stmt);
    }
    if (step != SQLITE_DONE) {
        BatchFail(interp, handle, "MAPPING", -1, NULL, sqlite3_errmsg(db));
        goto finish;
    }
    rc = sqlite3_finalize(stmt);
    stmt = NULL;
    if (rc != SQLITE_OK) {
        BatchFail(interp, handle, "MAPPING", -1, NULL, sqlite3_errmsg(db));
        goto finish;
    }
    for (size_t c = 0; c < mapped; c++) {
        const Ns_DbBatchColumn *field = Ns_DbBatchGetColumn(batch, map[c].source);
        bool binary = field->refinement != NULL && strcmp(field->refinement, "encoded_binary") == 0;
        bool nativeTime = field->refinement != NULL
                          && strcmp(field->refinement, "accesslog_time") == 0
                          && (affinity[c] == B_AFF_INTEGER || affinity[c] == B_AFF_NUMERIC);
        bool text = strcmp(field->type, "string") == 0 && !binary && !nativeTime;
        bool lexicalNumber = strcmp(field->type, "number") == 0 && field->refinement == NULL
                             && affinity[c] == B_AFF_TEXT;

        if (!found[c]) {
            BatchFail(interp, handle, "MAPPING", -1, map[c].destination,
                      "destination column not found");
            goto finish;
        }
        if (strcmp(field->type, "null") != 0 && !lexicalNumber
            && ((binary && affinity[c] != B_AFF_BLOB)
                || (text && affinity[c] != B_AFF_TEXT && affinity[c] != B_AFF_BLOB)
                || (!text && !binary
                    && (affinity[c] == B_AFF_REAL || affinity[c] == B_AFF_TEXT)))) {
            BatchFail(interp, handle, "TYPE_MISMATCH", -1, map[c].destination,
                      "target affinity can coerce or lose the requested type/precision");
            goto finish;
        }
    }
    Tcl_DStringSetLength(&sql, 0);
    Tcl_DStringAppend(&sql, "INSERT INTO ", -1);
    BatchQuote(&sql, database);
    Tcl_DStringAppend(&sql, ".", 1);
    BatchQuote(&sql, tableName);
    Tcl_DStringAppend(&sql, " (", 2);
    for (size_t c = 0; c < mapped; c++) {
        if (c) {
            Tcl_DStringAppend(&sql, ",", 1);
        }
        BatchQuote(&sql, map[c].destination);
    }
    Tcl_DStringAppend(&sql, ") VALUES (", -1);
    for (size_t c = 0; c < mapped; c++) {
        Tcl_DStringAppend(&sql, c ? ",?" : "?", -1);
    }
    Tcl_DStringAppend(&sql, ")", 1);
    Tcl_UtfToExternalDString(encoding, sql.string, sql.length, &utf);
    if ((size_t)utf.length != strlen(utf.string)
        || sqlite3_prepare_v2(db, utf.string, utf.length, &stmt, NULL) != SQLITE_OK) {
        BatchFail(interp, handle, "MAPPING", -1, NULL, sqlite3_errmsg(db));
        goto finish;
    }
    for (size_t r = 0; r < Ns_DbBatchRowCount(batch); r++) {
        current = (Tcl_WideInt)r;
        for (size_t c = 0; c < mapped; c++) {
            Ns_DbBatchValue         v;
            const Ns_DbBatchColumn *field = Ns_DbBatchGetColumn(batch, map[c].source);
            sqlite3_int64           integer;
            bool                    micros = field->refinement != NULL
                                             && (strcmp(field->refinement, "duration_us") == 0
                                                 || strcmp(field->refinement, "datetime_us") == 0);

            column = field->name;
            if (Ns_DbBatchGetValue(batch, r, map[c].source, &v) != NS_OK || v.length > INT_MAX) {
                BatchFail(interp, handle, "CONVERSION", current, column,
                          "invalid or oversized provider value");
                goto finish;
            }
            if (v.isNull) {
                rc = sqlite3_bind_null(stmt, (int)c + 1);
            } else if (strcmp(field->type, "string") == 0 && field->refinement != NULL
                       && strcmp(field->refinement, "encoded_binary") == 0) {
                size_t         n = v.length / 2u;
                unsigned char *blob = ns_malloc(n == 0u ? 1u : n);

                if (v.length % 2u != 0u) {
                    ns_free(blob);
                    BatchFail(interp, handle, "CONVERSION", current, column, "invalid hex length");
                    goto finish;
                }
                for (size_t j = 0; j < n; j++) {
                    int a = BatchHex(v.bytes[j * 2u]), b = BatchHex(v.bytes[j * 2u + 1u]);

                    if (a < 0 || b < 0) {
                        ns_free(blob);
                        BatchFail(interp, handle, "CONVERSION", current, column,
                                  "invalid hex value");
                        goto finish;
                    }
                    blob[j] = (unsigned char)((a << 4) | b);
                }
                rc = sqlite3_bind_blob(stmt, (int)c + 1, blob, (int)n, SQLITE_TRANSIENT);
                ns_free(blob);
            } else if (field->refinement != NULL && strcmp(field->refinement, "accesslog_time") == 0
                       && (affinity[c] == B_AFF_INTEGER || affinity[c] == B_AFF_NUMERIC)) {
                if (!BatchTimestamp(v.bytes, v.length, &integer)) {
                    BatchFail(interp, handle, "CONVERSION", current, column,
                              "invalid or out-of-range access-log timestamp");
                    goto finish;
                }
                rc = sqlite3_bind_int64(stmt, (int)c + 1, integer);
            } else if (strcmp(field->type, "string") == 0
                       || (strcmp(field->type, "number") == 0 && field->refinement == NULL
                           && affinity[c] == B_AFF_TEXT)) {
                rc = sqlite3_bind_text(stmt, (int)c + 1, (const char *)v.bytes, (int)v.length,
                                       SQLITE_STATIC);
            } else {
                if (strcmp(field->type, "boolean") == 0) {
                    if (v.length == 4u && memcmp(v.bytes, "true", 4u) == 0) {
                        integer = 1;
                    } else if (v.length == 5u && memcmp(v.bytes, "false", 5u) == 0) {
                        integer = 0;
                    } else {
                        BatchFail(interp, handle, "CONVERSION", current, column, "invalid boolean");
                        goto finish;
                    }
                } else if (!BatchInteger(v.bytes, v.length, micros, &integer)
                           || (field->refinement != NULL && strcmp(field->refinement, "int32") == 0
                               && (integer < INT32_MIN || integer > INT32_MAX))) {
                    BatchFail(
                        interp, handle, "CONVERSION", current, column,
                        "integer overflow, unsupported syntax or inexact microsecond conversion");
                    goto finish;
                }
                rc = sqlite3_bind_int64(stmt, (int)c + 1, integer);
            }
            if (rc != SQLITE_OK) {
                BatchFail(interp, handle, "INSERT", current, column, sqlite3_errmsg(db));
                goto finish;
            }
        }
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            BatchFail(interp, handle, "INSERT", current, NULL, sqlite3_errmsg(db));
            goto finish;
        }
        *inserted += sqlite3_changes64(db);
        if (sqlite3_reset(stmt) != SQLITE_OK || sqlite3_clear_bindings(stmt) != SQLITE_OK) {
            BatchFail(interp, handle, "INSERT", current, NULL, sqlite3_errmsg(db));
            goto finish;
        }
    }
    rc = sqlite3_finalize(stmt);
    stmt = NULL;
    if (rc != SQLITE_OK) {
        BatchFail(interp, handle, "INSERT", current, NULL, sqlite3_errmsg(db));
        goto finish;
    }
    connection->affected = *inserted;
    connection->nrows = *inserted;
    result = TCL_OK;
finish:
    if (stmt != NULL) {
        sqlite3_finalize(stmt);
    }
    Tcl_FreeEncoding(encoding);
    ns_free(affinity);
    ns_free(found);
    Tcl_DStringFree(&sql);
    Tcl_DStringFree(&utf);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * DbBatchOps --
 *
 *      Return the native SQLite batch operations for driver registration.
 *
 * Results:
 *      Pointer to the immutable driver operations table.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static const Ns_DbBatchDriverOps *
DbBatchOps(void)
{
    return &batchOps;
}
#endif


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
    if (DbConfigureDatasources(driver) != NS_OK) {
        return NS_ERROR;
    }
    Ns_Log(Notice, "nsdbsqlite: version %s loaded, based on SQLite %s (headers %s)",
           DRIVER_VERSION, sqlite3_libversion(), SQLITE_VERSION);
    return NS_OK;
}

/* nsdb loads the driver before copying datasource values into its pools. */
static Ns_ReturnCode
DbConfigureDatasources(const char *driver)
{
    const Ns_Set *pools = Ns_ConfigGetSection("ns/db/pools");
    size_t i;
    bool directoryReady = NS_FALSE;

    for (i = 0u; pools != NULL && i < Ns_SetSize(pools); i++) {
        const char *section = Ns_ConfigGetPath(NULL, NULL, "db", "pool",
                                               Ns_SetKey(pools, i), NS_SENTINEL);
        const char *poolDriver, *source;
        Tcl_DString path, normalized;

        if (section == NULL) {
            continue;
        }
        poolDriver = Ns_ConfigGetValue(section, "driver");
        if (poolDriver == NULL || !STREQ(poolDriver, driver)) {
            continue;
        }
        source = Ns_ConfigGetValue(section, "datasource");
        /* Missing datasources are rejected by nsdb. Preserve SQLite's special
         * names, URI spelling, and explicitly supplied absolute paths.
         */
        if (source == NULL || *source == '\0' || STREQ(source, ":memory:")
            || strncmp(source, "file:", 5u) == 0 || Ns_PathIsAbsolute(source)) {
            continue;
        }
        Tcl_DStringInit(&path);
        Tcl_DStringInit(&normalized);
        if (!directoryReady) {
            struct stat st;
            int level;

            /* Only provision the standard root, never datasource subdirectories. */
            for (level = 0; level < 2; level++) {
                Ns_MakePath(&path, Ns_InfoHomePath(), "data",
                            level == 0 ? NULL : "sqlite", NS_SENTINEL);
                if (Ns_RequireDirectory(Tcl_DStringValue(&path)) != NS_OK
                    || !Ns_Stat(Tcl_DStringValue(&path), &st)
                    || !S_ISDIR(st.st_mode)) {
                    Ns_Log(Error, "nsdbsqlite: required data directory '%s' is unavailable",
                           Tcl_DStringValue(&path));
                    Tcl_DStringFree(&normalized);
                    Tcl_DStringFree(&path);
                    return NS_ERROR;
                }
                Tcl_DStringSetLength(&path, 0);
            }
            directoryReady = NS_TRUE;
        }
        Ns_MakePath(&path, Ns_InfoHomePath(), "data", "sqlite", source, NS_SENTINEL);
        Ns_NormalizePath(&normalized, Tcl_DStringValue(&path));
        Ns_SetIUpdate(Ns_ConfigGetSection(section), "datasource",
                      Tcl_DStringValue(&normalized));
        Tcl_DStringFree(&normalized);
        Tcl_DStringFree(&path);
    }
    return NS_OK;
}

static int DbInterpInit(Tcl_Interp * interp, const void *UNUSED(arg))
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

/* SQLite already found the statement boundary; inspect only its unused tail.
 * Do not prepare the tail: some PRAGMAs have effects during preparation.
 */
static bool
DbSqlTailIsEmpty(const char *tail)
{
    const char *p = tail;

    while (*p != '\0') {
        /* SQLite's ASCII whitespace plus empty statement separators. */
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'
            || *p == '\f' || *p == ';') {
            p++;
        } else if (p[0] == '-' && p[1] == '-') {
            p += 2;
            while (*p != '\0' && *p != '\n') {
                p++;
            }
        } else if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p != '\0' && !(p[0] == '*' && p[1] == '/')) {
                p++;
            }
            /* SQLite accepts a block comment continuing to end of input. */
            if (*p != '\0') {
                p += 2;
            }
        } else {
            return NS_FALSE;
        }
    }
    return NS_TRUE;
}

/* Shared preparation keeps ordinary execution and the sp_* shim in sync. */
static Ns_ReturnCode
DbPrepare(Ns_DbHandle *handle, const char *sql)
{
    Connection *connectionPtr = (Connection *)handle->connection;
    Context *contextPtr;
    const char *tail;
    int rc;

    if (DbCancel(handle) != NS_OK) {
        return NS_ERROR;
    }
    contextPtr = ns_calloc(1, sizeof(Context));
    connectionPtr->nrows = 0;
    connectionPtr->affected = 0;
    contextPtr->totalBefore = sqlite3_total_changes64(connectionPtr->db);
    handle->statement = contextPtr;

    rc = sqlite3_prepare_v2(connectionPtr->db, sql, -1, &contextPtr->stmt, &tail);
    if (rc != SQLITE_OK) {
        DbError(handle, "preparing SQL");
        DbCancel(handle);
        return NS_ERROR;
    }
    if (contextPtr->stmt == NULL) {
        Ns_DbSetException(handle, "NSDB", "no SQL statement");
        DbCancel(handle);
        return NS_ERROR;
    }
    if (!DbSqlTailIsEmpty(tail)) {
        Ns_DbSetException(handle, "NSDB", "SQL after the first statement is not supported");
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
DbObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const objv[])
{
    Ns_DbHandle         *handle;
    static const char *const opts[] = {
        "rows_affected", "version", NULL
    };
    enum {
        IRowsAffectedIdx, IVersionIdx
    } opt;

    if (objc < 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "option handle");
        return TCL_ERROR;
    }

    if (Tcl_GetIndexFromObj(interp, objv[1], opts, "option", 1, (int *) &opt) != TCL_OK) {
        return TCL_ERROR;
    }

    if (Ns_TclDbGetHandle(interp, Tcl_GetString(objv[2]), &handle) != TCL_OK) {
        return TCL_ERROR;
    }

    assert(handle != NULL);
    /* Validate the driver before interpreting its private connection data. */
    if (!STREQ(Ns_DbDriverName(handle), DbName())) {
        Tcl_AppendResult(interp, "handle \"", Tcl_GetString(objv[2]),
                         "\" is not of type \"", DbName(), "\"", NULL);
        return TCL_ERROR;
    }

    /* Both subcommands take only the validated handle, with no trailing args. */
    if (Ns_ParseObjv(NULL, NULL, interp, 3, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    switch (opt) {
    case IRowsAffectedIdx:
        Tcl_SetObjResult(interp,
                         Tcl_NewWideIntObj((Tcl_WideInt)((Connection *)handle->connection)->affected));
        break;

    case IVersionIdx:
        Tcl_SetObjResult(interp, Tcl_NewStringObj(sqlite3_libversion(), TCL_INDEX_NONE));
        break;
    }
    return TCL_OK;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
