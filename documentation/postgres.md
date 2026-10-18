# The Postgres backend

**Status:** describes what shipped in story S4 (2026-10-18).

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
its own, never the data store's (AD-13, AD-19). What Postgres keeps of a
destroyed key - its dead tuple until vacuum, the WAL and any archive of it -
is beyond this library; that is why the key store is a port of its own with
backups of its own.

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

## 6. Building and testing

libpq is found with `pkg-config libpq` and only inside the build image this
repository's `Containerfile` defines (`ghoti-build:gcc16` plus the pinned
`libpq-dev`); it is never installed on a developer host. Without it the
Makefile stops and names the image.

`make test` needs a server. `tools/pg-test.sh` starts one: the spine's
`docker.io/library/postgres@sha256:885953...` (18.6, linux/amd64) in podman or
docker on `127.0.0.1` and a random port, waits for it, runs a command in the
build image on the host's network with `GDEFSPG_TEST_CONNINFO` set, and removes
the server on every exit path. Without a server the test programs fail before
any test, naming that script; with `GDEFSPG_TEST_SKIP_SERVER=1` they skip, print
`SKIPPED: postgres`, and gtest counts their tests as skipped, never passed
(`make test` ends with the same line). `testHarness` checks those three
behaviours by running itself, and needs no server.

Locally, with the build image made once:

```
podman build -t ghoti-build-pg:gcc16-libpq17.11 -f Containerfile .
tools/pg-test.sh -- 'make test PREFIX=$PWD/../../.local-defiant-storage-postgres'
```

(the prefix holding its dependencies, built in the same image). On the EVO-X2
the build container has no container engine, so the tests run in the job's
host phase (`tools/evo run --host-after`), which has docker:

```
tools/evo image build localhost/ghoti-build-pg:gcc16-libpq17.11 DIR DIR/Containerfile
tools/evo run JOB --image localhost/ghoti-build-pg:gcc16-libpq17.11 \
  --host-after 'sh src/defiant-storage-postgres/tools/pg-test.sh --pull-and-remove \
    --image localhost/ghoti-build-pg:gcc16-libpq17.11 --mount "$JOB:/job" --workdir /job \
    -- "make -C src/defiant-storage-postgres test PREFIX=/job/prefix"' \
  -- '<install the dependencies into $PREFIX>'
```

`--pull-and-remove` pulls the server image by its digest and removes it again
when the run ends, so nothing of the run is left on the box.

| Program | What |
| --- | --- |
| `testModel` | the port's conformance suite (`model.inc`, the purchase-order fixture) and the key store suite (`keystore.inc`), every query tee'd against `gdefstor_model_scan()` |
| `testAgree` | a seeded sweep against the reference scan, the memory backend and the SQLite backend; derived tables wiped from outside and rebuilt |
| `testPool` | two tenants on one pooled connection carry nothing; a failed statement's connection is never used again; a step meets a lock and fails; refused schemas; names |
| `testIndex` | concurrent builds outside the step; a failed build's invalid index dropped and rebuilt |
| `testScale` | 10,000 orders: `EXPLAIN` of every compiled statement names its index |
| `testDialect` | the dialect, and text by bytes on a database whose collation is not |
| `testHarness` | no server, a server that does not answer, the skip flag |

`make check-pg` plants a session-level `search_path`, `COLLATE "C"` dropped, a
connection reused after an error, an index build inside a transaction and a
skipped arm reported as passing, and requires each named test to fail.
