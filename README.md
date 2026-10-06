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
