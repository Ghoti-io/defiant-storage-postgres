# defiant-storage-postgres

The Postgres backend for the Defiant storage port: the entity port
(`defiant-storage`'s `model.h`) and the key store port, served through the
shared schema generator and query compiler, `defiant-storage-sql`. This
library is the Postgres dialect and a libpq executor: per-tenant schemas,
schema-qualified statements, a connection pool that carries no tenant state,
prepared statements per tenant, and index builds with `CREATE INDEX
CONCURRENTLY` outside the step that declares them.

```c
#include <ghoti.io/defiant-storage-postgres/defiant-storage-postgres.h>

GDEFSPG_Options o;
gdefspg_options_default(&o);
o.conninfo = "host=db.internal dbname=site user=defiant";
GDEFSTOR_Store * store = NULL;
if (gdefspg_model_store_create(&o, NULL, NULL, &store) == GDEFSPG_OK) {
  /* gdefstor_type_declare(), gdefstor_entity_save(), gdefstor_entity_query() ... */
  gdefstor_store_destroy(store);
}
```

## Building

It depends, through pkg-config, on `defiant-storage`, `defiant-storage-sql`
and `libpq`. libpq is built against only inside the image the `Containerfile`
here defines (`ghoti-build:gcc16` with `libpq-dev` pinned at 17.11, which the
gcc base image already carries), never on a developer host; without it the
Makefile stops and says so. `make test` needs a throwaway Postgres server,
which `tools/pg-test.sh` starts (the pinned `postgres:18` image by digest,
PostgreSQL 18.6); `GDEFSPG_TEST_SKIP_SERVER=1` skips the Postgres arm and says
`SKIPPED: postgres`, and each program that needs a server reports one test
skipped and none passed.
`documentation/postgres.md` has the commands, including for a build host
whose build container has no container engine.

## The API

`gdefspg_model_store_create()` opens a store over one database;
`gdefspg_model_backend()` gives the entity backend alone, for a caller that
wraps it; `gdefspg_keystore_create()` opens a key store over a database of its
own. `GDEFSPG_Options` holds the connection string, the schema prefix,
`lock_timeout`, the pool and cache sizes, and observers for statements, idle
connections and refusals. `gdefspg_dialect()` and `gdefspg_schema_name()` are
for tests and diagnostics.

## Status

Built in story S4 of the storage rewrite, with records, counters and the
outbox (S6a) and snapshot restore (S6a3); it stores the SQL layer's schema at
version 1. Nothing is released; the interface is `@stability free`.
