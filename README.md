# SQLite Database Driver for NaviServer

**Release:** 0.9  
**Author:** [Vlad Seryakov](mailto:vlad@crystalballinc.com)

This is a NaviServer module providing a database driver for accessing [SQLite](http://www.sqlite.org) databases.

The driver is based on **nssqlite3** from AOLserver 4.5 by  
[Dossy Shiobara](mailto:dossy@panoptic.com)

---

## Compiling and Installing

The driver builds with its bundled SQLite source; a separate SQLite installation
is not required.
If NaviServer is installed in the default location (`/usr/local/ns`)

```bash
make && sudo make install
```

Otherwise, provide a path to the install location, like e.g.

```bash
make NAVISERVER=/opt/local/ns499/
sudo make NAVISERVER=/opt/local/ns499/ install
```


---

## Configuration Snippet

```tcl
ns_section ns/db/drivers {
    ns_param sqlite       nsdbsqlite.so
}

ns_section ns/db/pools {
    ns_param sqlite       "SQLite"
}

ns_section ns/db/pool/sqlite {
    ns_param driver       sqlite
    ns_param connections  1
    ns_param datasource   mydatabase.db
    ns_param verbose      off
}
```

## Standalone database layout

Use `<NaviServer home>/data/sqlite/<database>.db` for persistent databases,
where the name identifies the logical database, independently of the application
or connection pool. For example:

```text
/usr/local/ns/data/sqlite/
    openacs-org.db
    another-site.db
    mail.db
```

SQLite has no separate cluster service. Each database file can contain many
tables and serve multiple applications. Use table prefixes such as `smtpd_`
when applications share a database. Several NaviServer pools or virtual servers
can point at the same file; adding a pool does not require a new database.

A relative `datasource`, such as `mail.db` or `sites/example.db`, is completed
under `<NaviServer home>/data/sqlite/` and normalized. `ns_db datasource $h`
reports the resolved path. An absolute path is used unchanged. `:memory:`, the
empty string (SQLite's temporary database), and `file:` URI strings keep their
existing SQLite behavior; this completion does not enable URI processing.

For example, `ns_param datasource mail.db` uses the logical database
`<NaviServer home>/data/sqlite/mail.db`, regardless of the pool name. The
`datasource` parameter remains required; the driver does not invent a database
name when it is omitted. For relative datasources, driver initialization ensures
that `<home>/data` and `<home>/data/sqlite` exist, reporting an error if creation
fails. Provision custom absolute directories and relative subdirectories before
opening their pools; the driver does not create those directories. Existing configurations with relative
paths must use an absolute path to preserve their previous location.

Keep related tables together when operations need a common transaction. Choose
separate files when datasets have independent ownership, backup, or maintenance
requirements. OpenACS's primary database requirements are unchanged; SQLite can
provide accompanying standalone services such as nssmtpd persistence.

Keep database files outside the page root. The server account needs write access
to the containing directory and the database, since SQLite creates journal,
WAL, and shared-memory files alongside it. The server account also needs permission
to create the standard data directories if missing. Installers or Docker volume
provisioning can create them in advance. SQLite creates the database on its first open.
Use a persistent local volume and include these databases in the site's backup
and recovery procedures. Back up live databases with SQLite's backup facilities
or after a clean shutdown rather than copying a database file during writes.

For a modest standalone service, start with one connection per pool. Configure
foreign-key enforcement, lock waiting, and other connection settings consistently
for every connection, including reopened connections. Choose journal and
synchronous settings according to the service's durability requirements; the
driver does not silently select WAL or change SQLite's durability defaults.

---

## Minimal Example

With the default home from the configuration above, create the database with
`sqlite3 /usr/local/ns/data/sqlite/mydatabase.db` and populate it with values.
The SQLite command-line program is needed only for this interactive example:

```sql
create table t1 (c INT);
insert into t1(c) values (1), (2), (3);
select * from t1;
```

Start NaviServer using the configuration snippet above.

Count the number of tuples in `t1`:

```tcl
set h [ns_db gethandle sqlite]
set s [ns_db 1row $h "select count(*) from t1"]
ns_db releasehandle $h
ns_log notice RESULT [ns_set format $s]
```

Insert one more tuple into the database:

```tcl
set h [ns_db gethandle sqlite]
set r [ns_db dml $h "insert into t1(c) values (4)"]
ns_db releasehandle $h
```

---

## Tests

Run `make test` (or `make NAVISERVER=/path/to/ns test`). The tests include direct C callback/finalization checks and start
an isolated NaviServer instance without a network listener and use a temporary
database. `TESTFLAGS` accepts tcltest options, for example `-verbose bpse`.


## Pooled transactions

Finish successful transactions with `COMMIT` before releasing a handle.
Returning a handle to the pool finalizes pending statements and rolls back any
unfinished transaction, including one opened by an outermost `SAVEPOINT`.
`ns_db resethandle $h` performs the same cleanup explicitly. Committed changes
are preserved. Reset reports SQLite errors if rollback fails.

## Refreshing bundled SQLite

The refresh target downloads a release tag from SQLite's official Git mirror
at <https://github.com/sqlite/sqlite>, configures the upstream source, and
builds its amalgamation. `SQLITE_VERSION` defaults to `3.53.4`:

```bash
make refresh-sqlite
# Select a different release explicitly:
make refresh-sqlite SQLITE_VERSION=3.53.4
make test
```

Refreshing requires network access, `curl`, `tar`, `make`, and the upstream
SQLite build tools (a C compiler and the tools required by its configure/build
scripts). It does not require a Tcl source distribution. `CURL` can override
the download command.

Both `sqlite3.c` and `sqlite3.h` are generated in a temporary directory and
validated against the requested version before replacing local files. Their
upstream public-domain notices are preserved. Identical files retain their
timestamps. Downloads and generation are explicit maintenance operations;
normal builds use the checked-in files and require no network access.

Review the resulting diff and run `make test` before committing a refresh.

## Row counts

`ns_db rowcount $h` reports directly affected rows for successful
INSERT/UPDATE/DELETE statements. For SELECT, it reports rows fetched so far,
including after reaching the end of the result or flushing a partial result.
SQLite streams results, so the total is not known before fetching.

`ns_sqlite rows_affected $h` reports direct modifications by the current or
most recently executed statement, excluding trigger and foreign-key side
effects. It remains available after statement cleanup. SELECT, DDL and
transaction commands report zero affected rows. Statements with `RETURNING`
publish their affected count after fetching to completion.

Both counters start at zero for a new SQL operation or a reset/released handle.
They are connection-local. `ns_sqlite rows_affected` supports 64-bit counts;
`ns_db rowcount` returns an error value (-1) if the count exceeds its integer
interface's range.


## Version information

On NaviServer 5 and later, `ns_db info $h` includes `clientversion` and
`serverversion`, matching the keys used by nsdbpg. Both report SQLite's runtime
library version because its client library and database engine run in the same
process. The numeric encoding is `major * 1000000 + minor * 1000 + patch`;
SQLite 3.53.4 is reported as `3053004`.

`ns_sqlite version $h` continues to return the version string. Driver startup
also logs the driver version, runtime SQLite version, and compile-time header
version. Inspecting versions does not execute SQL or alter results or counts.

## Legacy SQLite statistics

```tcl
ns_sqlite stats $handle
ns_sqlite stats $handle -database attached_database
```

This compatibility interface retains its existing `connection`, `statement`,
`wal` and `policy` dictionaries, counter `{available current highwater}` wrappers,
statement availability flag, and WAL `available`, `resultCode` and `-1` sentinels.
When compiled against the new core, the additive `driver`/`supported` fields also
remain. Existing names and value representations are unchanged.

`-database` selects the attached database for the WAL snapshot and defaults to
`main`. Unknown or empty names are errors. Connection counters cover the whole
connection, regardless of this option. Policy describes the main pool datasource.

Statistics use engine counters and read-only policy PRAGMAs without fetching or
finalizing pending results, resetting counters, changing affected-row counts or
owning application transactions. WAL sampling uses CHECKPOINT_NOOP and does not
copy frames, force a checkpoint or change automatic checkpoint policy.

### Connection counters

Each connection entry contains `available`, and, when supported, `current` and
`highwater` values from `sqlite3_db_status64()`. Values are returned as Tcl wide
integers. `available false` means SQLite does not support that counter.

| Entry | Meaning and units |
| --- | --- |
| `cacheUsedBytes` | Current pager-cache memory, bytes |
| `cacheUsedSharedBytes` | Cache memory with shared caches apportioned between connections, bytes |
| `cacheHits`, `cacheMisses` | Cumulative pager-cache hits and misses (`current`) |
| `cacheWrites` | Cumulative successful dirty-page writes (`current`); in WAL mode these are writes to the WAL, not subsequent checkpoint writes |
| `cacheSpills` | Cumulative dirty-page spills caused by cache overflow (`current`) |
| `schemaUsedBytes`, `statementUsedBytes` | Current memory used for schemas and prepared statements, bytes |
| `deferredForeignKeys` | Whether unresolved deferred foreign-key constraints exist (`current`, 0 or 1) |
| `lookasideUsed` | Current and peak allocated lookaside slots |
| `lookasideHits` | Cumulative successful lookaside allocations (`highwater`) |
| `lookasideMissSize`, `lookasideMissFull` | Cumulative lookaside misses due to allocation size or exhausted slots (`highwater`) |
| `tempBufferSpillBytes` | Cumulative temporary-buffer spill bytes (`current`) |

SQLite uses `current` or `highwater` differently for different counters. Unused
fields are zero; in particular cache hit/miss/write/spill totals are in `current`,
whereas lookaside allocation totals are in `highwater`. Gauge values such as
cache memory should not be interpreted as monotonically increasing counters.

Counters belong to the physical SQLite connection. They are not reset when a
handle is returned to the pool, but reconnecting creates new counters. Compare
snapshots only across a known continuous connection lifetime. A separately
checked-out reader does not report the writer's cache counters. Sampling the
active writer must be performed by that writer, while it owns its handle.

### Pending-statement counters

`statement available` indicates whether the driver currently has a prepared
statement. When available, the section includes `fullscanSteps`, `sorts`,
`autoindexRows`, `vmSteps`, `reprepares`, `runs` and `memoryBytes`, read using
`sqlite3_stmt_status()` without resetting. These cover only that pending
statement and the work already performed. They are not cumulative totals of
previously completed queries; after finalization the statement is unavailable.
SQLite's statement execution counters use its integer counter widths.

### WAL progress

`wal` contains `database`, `available`, `resultCode`, `logFrames` and
`checkpointedFrames`. The driver uses `SQLITE_CHECKPOINT_NOOP`: it reports the
current WAL frame counts without copying frames into the database. A database
outside WAL mode has `available false` and frame counts of -1. If SQLite cannot
obtain a snapshot, `resultCode` reports the SQLite error and `available` is false;
connection and statement statistics are still returned.

When available, `logFrames - checkpointedFrames` gives uncheckpointed WAL frames
at the observation. WAL frame counts can decrease when the WAL is reset or reused;
they are not cumulative write counters. These statistics do not report checkpoint
execution count, checkpoint duration, synchronization duration, or physical disk
bytes. They do not replace host I/O measurements.

For example, within code already holding the writer handle:

```tcl
set before [ns_sqlite stats $handle -database nsmcp_records]
# Perform the existing ingestion transaction here.
set after [ns_sqlite stats $handle -database nsmcp_records]
set writtenPages [expr {
    [dict get $after connection cacheWrites current]
    - [dict get $before connection cacheWrites current]
}]
```

The commands return dictionaries directly and can be used in nscp without `puts`.
A rebuilt driver and server restart are required; this is not a Tcl-only reload.

## Statement lifecycle

Binding or fetching after a result has been canceled, flushed, exhausted, or
reset returns a driver error. Fetching requires a row set with the same number
of fields as the result; a mismatched set is rejected without consuming a row.
Cancel and flush are safe to repeat and clear the pending statement even if
SQLite finalization reports an error. Existing SQL diagnostics are preserved.

Statement replacement stops if finalizing the previous statement fails. Reset
still attempts to roll back an unfinished transaction, and close still attempts
to close the connection; both report cleanup errors to their callers.

## Stored-procedure compatibility

SQLite has no native stored procedures. This driver supports the ns_db
`sp_start`/`sp_exec` pair as a compatibility shim for SQL statements:

```tcl
ns_db sp_start $h {SELECT 1 AS value}
# sp_start prepares SQL and returns 0 (NS_OK).
ns_db sp_exec $h
# Returns NS_ROWS; bind and fetch the result normally.
set row [ns_db bindrow $h]
while {[ns_db getrow $h $row]} {
    ns_log notice "value: [ns_set get $row value]"
}
```

For DML, `sp_start` prepares without stepping the statement; `sp_exec` executes
it and returns `NS_DML`. Statements with results return `NS_ROWS` and execute
as rows are fetched, like ordinary SELECT/RETURNING calls. Preparation errors
are reported by `sp_start`; execution errors are reported by `sp_exec` or
fetching. Row counts follow the ordinary execution rules.

Each prepared shim can be executed once. A second `sp_exec`, or one without a
successful `sp_start`, returns an error. Cancel, flush, reset, pool release,
or preparing another statement discards pending execution. SQL requiring bind
parameters is rejected; `sp_setparam`, `sp_getparams`, and `sp_returncode` remain
unsupported. Use `ns_db exec`, `dml`, or `select` for ordinary SQL calls.

## SQL statement boundaries

`exec`, `dml`, `select`, the row helpers, and the `sp_start` compatibility shim
accept one executable SQL statement per call. Trailing whitespace, comments,
and empty semicolon separators are allowed. SQLite determines the boundary of
the first statement, so semicolons in strings, quoted identifiers, and trigger
bodies are handled correctly. Empty or comment-only input returns
`no SQL statement`.

Additional SQL or malformed trailing text is rejected before stepping the first
statement; it is never silently discarded. The tail is not prepared, since some
PRAGMAs change settings during preparation. As with SQLite generally, preparing
the first statement can itself have effects for certain PRAGMAs.

Unlike nsdbpg's `PQexec` batch execution, this driver exposes one statement and
one result per call. Send separate calls inside an explicit transaction when
several operations must succeed together.

---

## License

The driver and build/test files use the Mozilla Public License 2.0; see
LICENSE. SPDX-License-Identifier: MPL-2.0

The bundled SQLite source and header retain their upstream
copyright notices.

## Authors

* Dossy Shiobara - [dossy@panoptic.com](mailto:dossy@panoptic.com)
* Vlad Seryakov - [vlad@crystalballinc.com](mailto:vlad@crystalballinc.com)
* Gustaf Neumann



## Optional native batch ingestion

When compiled with the batch-capable nsdb headers/library, the driver registers
`ns_db batchinfo` / `ns_db batchinsert` callbacks through a `DbFn_Batch` accessor in its existing SQL driver
registration table. Older headers still build the ordinary driver without batch support;
a batch-capable nsdb explicitly rejects batch insertion for such a build.

The sink uses prepared bindings on the caller's checked-out connection and active
transaction, with checked integer/exact microsecond/UTC timestamp conversion at
the sink. Values remain lexical in the immutable batch. It validates target
column affinities to avoid silent numeric coercion and precision loss, quotes
identifier components, and always finalizes statements. Errors can leave earlier
rows in the outer transaction: callers must roll back. The sink never commits,
rolls back, consumes the batch or supplies a SQL/JSON fallback.

Supported scalar mappings include UTF-8 text, int32/int64, exact duration/instant
microseconds, access-log timestamp text or native UTC microseconds, booleans,
nulls and explicit hex-to-BLOB decoding. Generic JSON numeric lexemes can be
stored exactly in TEXT; unsupported refinements and lossy conversions fail.
Nested values and dictionary/JSON import are not implemented by this stage.

Build against the installed public NaviServer headers and libraries:

```sh
make NAVISERVER=/usr/local/ns
```

Batch support requires public `nsdb.h`/`batch.h` headers and a matching nsdb
library. No private core headers or source-directory include paths are needed.

The separate NaviServer development checkout used for the experiments contains
`nsdb/BATCH-IMPLEMENTATION.md`, which documents the API, C provider interface and
reproducible verification. Its
`tclsh tests/check-batches.tcl --fixture --sanitizers` runner covers the shared
interface, native SQLite sink, existing driver tests and independent all-row
fixture verification. No installation or production configuration changes are
performed by these tests. The driver amalgamation used here is SQLite 3.54.1.

Capability metadata uses ordinary Tcl string objects, without core atom IDs.
JSON numeric syntax is validated by the public `Ns_DbBatchGetValue` accessor in
nsdb; the external driver does not call private JSON helpers.
The sink retains its checked lexical-to-integer, exact microsecond and calendar
conversion routines. C batch readers return `NS_OK`/`NS_ERROR`.

## Native bind variables

With core `NS_DB_BIND_ABI`, this driver supports the shared bind-variable
interface. Use `ns_db bindinfo $handle -version` to check the core Tcl binding
API: version 2 introduced the source options below, version 3 added lists,
version 4 added rows, and version 5 added explicit binary values and row column
types. The current core reports version 5 when this driver is registered.
Use `-bindvars`, `-bindset $set` or `-binddict $dict` on the existing `ns_db`
`exec`, `dml`, `select`, `1row` and `0or1row` commands. Exactly one source is
allowed. Missing names are errors; explicit sources never fall back to variables.

The core transparently caches `:name` metadata in SQL Tcl objects. Values reach
SQLite through `sqlite3_bind_text` or `sqlite3_bind_null`, never SQL concatenation.
Empty text is preserved unless `-emptynull` explicitly maps it to SQL NULL.
Numeric Tcl objects retain quoted-text semantics and normal column affinity.
NUL SQL/text and unplanned/positional parameters are rejected.

Ordinary calls without binding options, result fetching and caller-owned
transactions are unchanged. Cached parameter metadata does not add a SQLite
prepared-statement cache. Older core headers omit this optional callback.
There are no public `bindplan` or `bindexec` commands. See core `ns_db` docs
for return shapes, option placement, quoting rules and cardinality checks.

### Bind type metadata

`-bindtypes {ids list}` expands `IN (:ids)` into native parameters while other
parameters default to `scalar`. The option works with variables, sets and
dictionaries. Empty or malformed lists are errors; `-emptynull` maps empty
elements to NULL but does not accept empty containers. List values are never
inserted as SQL text. Use the conventional colon notation for parameter names.
Core Tcl binding API version 3 identifies list support.

The core caches token positions and one expanded SQL shape per SQL object.
SQLite supplies numbered markers through the optional core driver hook and
binds them directly by index. Named C API callers retain verified slot/name
lookup paths. This avoids preparation and binding name searches for expanded
lists. SQLite still prepares SQL for
each execution; the shape cache is not a connection statement cache.

### Explicit binary binding

With core `NS_DB_BIND_ABI`, this driver registers one `DbFn_BindTyped`
callback for text and binary parameters. A NULL types array means all text;
there is no separate text wrapper. This requires the matching core generalized
dispatch. Core continues supporting other drivers with legacy text-only
callbacks. Core binding syntax version 5 adds
`-bindtypes {key binary}` and `-bindtypes {values {rows {binary scalar scalar}}}`.
SQLite uses `sqlite3_bind_blob64()` with the exact Tcl bytearray bytes and
`SQLITE_TRANSIENT`. Embedded NUL and all high bytes are preserved. Empty binary
is a zero-length BLOB; `-emptynull` explicitly maps it to NULL. Plain scalar
binding continues to use text, regardless of the Tcl internal representation.
No hex/base64 conversion or guessing based on object type occurs.

### Bound text encoding cache

Ordinary string values cache their externally encoded bytes in their Tcl object.
Repeated binding reuses these bytes while the value and system encoding remain
unchanged. Mutation or representation changes invalidate the cache; duplicates
share its reference-counted storage. SQLite copies the bytes before the call
returns. Numeric, list, dictionary, bytearray and other foreign representations
are preserved and use temporary encoded text; explicit binary values bypass
this text cache. Text containing NUL remains an error. SQL and parameter-value
caches do not create a connection-level prepared-statement cache.


### SQL array parameters

Core binding syntax version 6 adds native SQL `array` parameters. SQLite does
not support this interpretation. `ns_db bindinfo $h -types` reports
`scalar binary`; requesting `-bindtypes {ids array}` returns an explicit
unsupported-array error before execution. Use `list` for IN expansion or `rows`
for VALUES expansion instead; their existing semantics are unchanged.


### Per-pool connection policy

Configure SQLite policies directly in `ns/db/pool/POOL`, alongside `driver`,
`datasource` and `connections`. There is no separate `/sqlite` policy subsection.
Core nsdb reads its parameters and the selected driver reads its additional
parameters from the same section. This driver applies policies
when opening a native connection and restores them when its handle returns to
the pool. Applications select a pool and keep their own retention/schema/budget
settings; they need not issue baseline PRAGMAs. Policies address the primary
`main` database and connection-level options, not arbitrary attached databases.
Unspecified options retain SQLite defaults (including a file's persistent WAL mode).

| Parameter | Accepted values | Meaning |
| --- | --- | --- |
| journalMode | DELETE, TRUNCATE, PERSIST, MEMORY, WAL, OFF | Primary database journal mode |
| synchronous | OFF, NORMAL, FULL, EXTRA | Synchronization policy |
| walAutoCheckpointPages | 0..2147483647 | Automatic WAL checkpoint trigger; 0 disables it |
| busyTimeoutMs | 0..2147483647 | SQLite lock wait in milliseconds |
| cacheSizeKiB | 1..2147483647 | Cache target, applied as negative cache_size |
| tempStore | DEFAULT, FILE, MEMORY | Temporary storage policy |
| mmapSizeBytes | 0..9223372036854775807 | Memory map limit in bytes |
| journalSizeLimitBytes | -1..9223372036854775807 | Retained journal/WAL size limit |

Names are case-insensitive. Numeric values are integer counts, not unit strings.
Invalid values fail before opening a datasource. Unsupported or clamped values
fail effective-value verification rather than silently selecting another policy.
`ns_sqlite stats $handle` reports a `policy` dictionary; `cacheSize` is SQLite's
raw signed cache_size (negative KiB, positive pages), and `synchronousValue`
additionally reports the numeric synchronization level. Reading statistics does
not run a checkpoint. Normal reset rolls back unfinished work before restoring
policy; failed policy reset closes the native connection instead of reusing it.

WAL/NORMAL allows concurrent readers and one writer and preserves consistency,
but can lose recent commits on system failure. OFF can permit corruption. Pool
size and application recovery requirements must be chosen separately. Zero
checkpoint threshold requires an application/administrative checkpoint strategy.
Changing configuration takes effect on reconnect/reset; restart for consistent
policies across all worker interpreters and handles.


## Shared per-handle statistics

```tcl
ns_db stats $handle
ns_db stats $handle -database main
```

The optional `DbFn_Stats` callback returns the shared vocabulary below, with
`driver sqlite` and `supported true` supplied by core. All metric values are
scalars. Missing means unsupported or unavailable; zero remains a measured zero.
Empty groups are omitted. There are no per-counter or per-group `available`
wrappers in this interface. A supported callback need not provide every metric.

| Shared path | SQLite measurement | Scope |
| --- | --- | --- |
| connection cacheUsedBytes | SQLITE_DBSTATUS_CACHE_USED | Current pager-cache bytes across this connection |
| connection cacheHits / cacheMisses | SQLITE_DBSTATUS_CACHE_HIT / CACHE_MISS | Cumulative pager-cache accesses |
| statement executions | SQLITE_STMTSTATUS_RUN | Execution starts of the retained statement |
| statement rowsReturned | Driver fetch counter | Rows delivered so far; zero for statements without result columns |
| statement memoryUsedBytes | SQLITE_STMTSTATUS_MEMUSED | Current prepared-statement memory |

SQLite does not supply a total connection `memoryUsedBytes` measurement, so it
is omitted. Do not substitute process-wide SQLite memory totals. Statement
groups disappear once the statement is finalized, rather than retaining a
historical statement snapshot.

SQLite-specific measurements live under `backend sqlite`:

| Group | Fields | Meaning |
| --- | --- | --- |
| connection | cacheUsedSharedBytes, schemaUsedBytes, statementsUsedBytes | Current bytes, with SQLite's shared-cache accounting |
| connection | cacheWrittenPages, cacheSpilledPages | Cumulative pager page writes and writes caused by cache overflow |
| connection | temporarySpillBytes | Cumulative temporary-file spill bytes |
| connection | hasUnresolvedForeignKeys | Boolean, not an inferred constraint count |
| connection | lookasideUsedSlots | Current allocated lookaside slots |
| connection | lookasideHits, lookasideSizeMisses, lookasideFullMisses | Cumulative allocation attempts, using SQLite's highwater API output |
| statement | fullScanSteps, sortOperations, automaticIndexRows, vmSteps, recompilations | SQLite statement-instance counters, not server scan statistics |
| policy | Existing pool policy names | Effective main-datasource/connection settings, as in the legacy interface |
| wal | database, logFrames, checkpointedFrames | NOOP snapshot for the selected WAL database |

Only the lookaside-used gauge has a meaningful peak in the exposed connection
API. It appears at `highwater backend sqlite connection lookasideUsedSlots`.
SQLite's always-zero cache/memory highwater outputs are omitted. Lookaside hit
and miss totals are ordinary counters, not peak gauges.

For a non-WAL database, the WAL group is absent. A temporarily blocked or failed
WAL sample retains `database`, `resultCode` and `reason` under `backend sqlite wal`,
but omits frame counts. A successful sample retains measured zero frame counts
and omits redundant success flags. Connection counters and main-pool policy are
not restricted by the WAL database selector.

`ns_sqlite stats` uses the same sampling implementation but preserves the legacy
result contract; applications can migrate explicitly to the shared names.
Callback registration is conditional on `NS_DB_STATS_ABI`. Compiling against
older core headers retains the legacy command without requiring a new core
symbol. Older drivers on the new core report `supported false`. The no-handle
`ns_db stats` pool statistics are unchanged.
