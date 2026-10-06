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
# Use the same home value configured in ns/parameters.
set home /usr/local/ns
set dbdir [file join $home data sqlite]
file mkdir $dbdir

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
name when it is omitted. Create the data directory, including any relative
subdirectories, before opening a pool. Existing configurations with relative
paths must use an absolute path to preserve their previous location.

Keep related tables together when operations need a common transaction. Choose
separate files when datasets have independent ownership, backup, or maintenance
requirements. OpenACS's primary database requirements are unchanged; SQLite can
provide accompanying standalone services such as nssmtpd persistence.

Keep database files outside the page root. The server account needs write access
to the containing directory and the database, since SQLite creates journal,
WAL, and shared-memory files alongside it. The configuration example creates
the data directory explicitly; SQLite creates the database on its first open.
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

