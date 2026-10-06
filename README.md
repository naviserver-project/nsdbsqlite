# SQLite Database Driver for NaviServer 4.x

**Release:** 0.9  
**Author:** [Vlad Seryakov](mailto:vlad@crystalballinc.com)

This is a NaviServer module providing a database driver for accessing [SQLite](http://www.sqlite.org) databases.

The driver is based on **nssqlite3** from AOLserver 4.5 by  
[Dossy Shiobara](mailto:dossy@panoptic.com)

---

## Compiling and Installing

To compile this driver, you must have SQLite3 installed.
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
    ns_param datasource   /tmp/sqlite.db
    ns_param verbose      off
}
```

---

## Minimal Example

Create a database in `/tmp/sqlite.db` and populate it with values:

```sql
.open /tmp/sqlite.db
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

## Authors

* Dossy Shiobara – [dossy@panoptic.com](mailto:dossy@panoptic.com)
* Vlad Seryakov – [vlad@crystalballinc.com](mailto:vlad@crystalballinc.com)

```

## Tests

Run `make test` (or `make NAVISERVER=/path/to/ns test`). The tests start
an isolated NaviServer instance without a network listener and use a temporary
database. `TESTFLAGS` accepts tcltest options, for example `-verbose bpse`.

## License

The driver and build/test files use the Mozilla Public License 2.0; see
LICENSE. The bundled SQLite source and header retain their upstream
public-domain notices.

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
