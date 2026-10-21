# The Postgres backend

**Status:** describes what is built: story S4 (2026-10-18), with records,
counters and the outbox (S6a) and the restore members a snapshot import
writes through (S6a3), over defiant-storage-sql's schema at version 1.

`defiant-storage-postgres` serves the entity port (`defiant-storage`'s
`model.h`) and the key store port (`keystore.h`) from Postgres, through
`defiant-storage-sql`. That library writes every statement; this one gives it
the Postgres dialect and a libpq executor, and owns the connections, the
prepared statements and the session settings.

## 1. Placement (AD-14)

One database holds every tenant's data store, each in a schema of its own:
the prefix (`GDEFSPG_Options::schema_prefix`, default `ds_`) and the lowercase
hex of the tenant's bytes, so tenant `t1` is `ds_7431` (`gdefspg_schema_name()`).
A schema name is at most 63 bytes; a tenant too long for that is
`GDEFSTOR_ERR_LIMIT`, never truncated. Every statement names its tables in that
schema. **The tenant is never connection state:** this library sets no
`search_path` and no other session setting a tenant could leave behind, so
any pooled connection serves any tenant. The first write of a tenant makes its
schema (`CREATE SCHEMA IF NOT EXISTS`) in the same transaction as its tables.

A key store is the same arrangement (default prefix `dk_`) in a database of
its own, never the data store's (AD-13, AD-19). A destroyed key is a dead
tuple until a vacuum, so after an erasure commits the key store runs
`VACUUM (FULL)` on `dsk_key` and `dsk_sealed`, outside the transaction, which
writes each table anew and removes the old pages (the dialect's `key_vacuum`;
a failed vacuum is reported, and the erasure stands). What the WAL and any
archive of it keep is beyond this library: **expiring WAL archives is the
operator's**, on the key store's own retention. That is why the key store is a
port of its own with backups of its own.

## 2. Connections and transactions

| What | How |
| --- | --- |
| a connection | `PQconnectdbParams` with the caller's conninfo, `client_encoding=UTF8`, and the session options: `-c lock_timeout=<GDEFSPG_Options::lock_timeout_ms>` (default 5000 ms; 0 is refused), added to any `options` the conninfo has |
| the pool | idle connections shared by every tenant (`idle_connections`, default 8) |
| after an error | the connection is closed, never pooled: a statement that failed, a commit that failed, a connection returned inside a transaction (AD-14) |
| prepared statements | per connection, keyed by tenant and statement text (`statement_cache`, default 64, least recently used evicted with `PQclosePrepared`) |
| a write | `BEGIN ISOLATION LEVEL SERIALIZABLE` |
| a read | `BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY`: one snapshot for a query's total and page |
| retryable | SQLSTATE `40001`, `40P01`, `55P03`, `57014` are `GDEFSTOR_ERR_BUSY` |

The session settings are connection options, applied by the server at
connect and the same on every connection. They are the "session settings"
this library adds; `lock_timeout` bounds every wait for a lock, that of a
schema step and that of a concurrent index build (AD-15).

Records, counters and the outbox (story S6a) are tables of the tenant's
schema written in the write's `SERIALIZABLE` transaction, so Postgres itself
keeps two writers of one tenant (two stores, or two nodes) from committing
the same counter value or holding live leases on one stream: one of two
transactions that read and change one row is stopped with `40001`
(`GDEFSTOR_ERR_BUSY`), and the caller tries again. `tests/test_system.cpp`
shows both with two stores on one schema, a thread each.

One case stays open, and is the reason the counter's rule is stated per
backend in defiant-storage-sql's `sql.md` ("Counters and aborts"): every
write runs inside a savepoint, and an abort of a write that took a counter
value rolls back to it and then commits the counter's floor. Rolling back to
a savepoint releases the counter's row, so a writer on another connection
that was waiting on that row can take the same value before the floor lands.
Values committed are never repeated; an aborted writer and a committed one
can have held the same number. Closing that needs a reservation outside the
write, which a Postgres sequence is and the shared dialect interface does not
have; it matters to a caller only if it uses a value of a write it then
aborts.

Values travel as untyped parameters, typed by the server from where each
stands: integers in the text format (so a boolean bound as an int64 lands in
its `SMALLINT` column), doubles, text and bytes in the binary format, results
in the binary format. Nothing reads the locale.

## 3. The dialect

| Member | Value |
| --- | --- |
| quotes, placeholders | `"`, `$1`, `$2`, ... |
| types | `BIGINT` for ids, integers, decimals (unscaled), dates and times; `DOUBLE PRECISION`; `SMALLINT` for booleans; `TEXT`; `BYTEA` |
| `text_collation` | `COLLATE "C"`: text compares and sorts by bytes whatever the database's collation |
| `nulls_low` | 0 (the compiler writes `NULLS FIRST` ascending) |
| upsert, `RETURNING` | `ON CONFLICT`, yes |
| limits | 63-byte identifiers, 65,535 parameters, 1,600 columns |
| `index_build` | `CONCURRENTLY ` |
| `index_valid` | a `pg_index.indisvalid` query by schema and index name |
| `index_drop` | `DROP INDEX CONCURRENTLY IF EXISTS ` (an invalid index only) |
| `qualifier_create` | `CREATE SCHEMA IF NOT EXISTS ` |
| `store_exists` | a `pg_class` query: does the schema exist, does it hold the version table, how many tables |

These strings, the four transaction statements and the session options are
the only SQL in this library; `make check-seams` refuses SQL text anywhere
else in `src/`.

## 4. What is refused

The first time an executor meets a tenant it asks `gdefssql_store_exists()`
and, when the store is there, `gdefssql_store_version()`. No schema (or an
empty one): a read holds nothing and makes nothing; a write makes the store.
A schema with tables but not this kind's version table (someone else's, or a
key store opened as a data store): `GDEFSTOR_ERR_FORMAT`. A version above this
build's: `GDEFSTOR_ERR_UNSUPPORTED`; below: `GDEFSTOR_ERR_FORMAT`. A refused
schema is left as it is, and the refusal is a sentence through
`GDEFSPG_Options::on_refusal` (or stderr).

## 5. Index builds (AD-15)

A schema step never holds `CREATE INDEX` (defiant-storage-sql's `sql.md`
section 3). After the declaring write commits, the SQL layer builds each new
index with `CREATE INDEX CONCURRENTLY` on the write's connection, outside any
transaction, and marks it built once `pg_index` says it is valid. Until then
the field reads `GDEFSTOR_INDEX_INDEXING`. A concurrent build that fails (it
waits for every older transaction, and gives up after `lock_timeout`) leaves an
invalid index; the next commit of the tenant drops it and builds it again.
Queries answer the same throughout.

The build runs in the committing call, after its commit: a declaration that
adds an index to a large table returns when the index is built, or when its
build gave up. Moving builds to a background worker is not built.

The blind table of a type with a personal field (`tN_b`, defiant-storage-sql's
`sql.md`) and its two indexes are steps like these: the table in the declaring
step, the indexes built concurrently after it. A MAC is bound as `bytea`.

## 6. Building and testing

libpq is found with `pkg-config libpq` and only inside the build image this
repository's `Containerfile` defines (`ghoti-build:gcc16` plus the pinned
`libpq-dev`); it is never installed on a developer host. Without it the
Makefile stops and names the image.

`make test` needs a server. `tools/pg-test.sh` starts one: the pinned
`docker.io/library/postgres@sha256:885953...` (18.6, linux/amd64) in podman or
docker on `127.0.0.1` and a random port, waits for it, runs a command in the
build image on the host's network with `GDEFSPG_TEST_CONNINFO` set, and removes
the server on every exit path. Without a server the test programs fail before
any test, naming that script; with `GDEFSPG_TEST_SKIP_SERVER=1` they skip, print
`SKIPPED: postgres`, and gtest reports one test (the arm's gate) skipped and
none passed (`make test` ends with the same line). `testHarness` checks those
three behaviors by running itself, and needs no server.

Locally, with the build image made once:

```
podman build -t ghoti-build-pg:gcc16-libpq17.11 -f Containerfile .
tools/pg-test.sh -- 'make test PREFIX=$PWD/../../.local-defiant-storage-postgres'
```

(the prefix holding its dependencies, built in the same image). Where the
dependencies are built inside a container that has no container engine of its
own, build them there first, then run `tools/pg-test.sh` from the host that
has the engine, mounting the same directory:

```
tools/pg-test.sh --pull-and-remove --engine docker \
  --image ghoti-build-pg:gcc16-libpq17.11 --mount "$DIR:/work" --workdir /work \
  -- 'make -C src/defiant-storage-postgres test PREFIX=/work/prefix'
```

`--pull-and-remove` pulls the server image by its digest and removes it again
when the run ends, so nothing of the run is left on the host.

| Program | What |
| --- | --- |
| `testModel` | the port's conformance suite (`model.inc`, the purchase-order fixture) and the key store suite (`keystore.inc`), every query tee'd against `gdefstor_model_scan()` |
| `testAgree` | a seeded sweep against the reference scan, the memory backend and the SQLite backend; derived tables wiped from outside and rebuilt; a tenant's snapshot carried memory -> SQLite -> Postgres -> memory, the truth byte-identical and every export the same bytes at each step |
| `testPool` | two tenants on one pooled connection carry nothing; a failed statement's connection is never used again; a step meets a lock and fails; refused schemas; names |
| `testIndex` | concurrent builds outside the step; a failed build's invalid index dropped and rebuilt |
| `testKeys` | an erasure, and a restore that replays one, vacuums the key tables outside the transaction and replaces their files |
| `testScale` | 10,000 orders: `EXPLAIN` of every compiled statement names its index |
| `testDialect` | the dialect, and text by bytes on a database whose collation is not |
| `testHarness` | no server, a server that does not answer, the skip flag |
| `testSystem` | records, counters and the outbox from two stores on one tenant schema, a thread each: no counter value committed twice, no two live leases on one stream |
| `testCore` | every result code has a string of its own; needs no server |

`make check-pg` plants a session-level `search_path`, `COLLATE "C"` dropped, a
connection reused after an error, an index build inside a transaction, a
skipped arm reported as passing and an erasure with no vacuum after it, and
requires each named test to fail.

defiant-core runs its own suites on this backend through the same script, each
a make target of core's that fails without a server: `test-keystore-postgres`
(the subject suite and the erasure sweep on the Postgres key store),
`test-listplan-postgres` (the compiled list and its sweeps),
`test-restore-postgres` (a tenant restored through core into Postgres),
`test-scale-postgres` (a list, a count and a create at 10,000 entities of one
type against 1,000, by the rows the server's own plans read, with a dropped
index planted) and `test-groups-postgres` (the purchase-order fixture through
core, with its plants).
