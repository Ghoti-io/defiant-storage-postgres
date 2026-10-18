# defiant-storage-postgres

The Postgres backend for the Defiant storage port: the entity port
(`defiant-storage`'s `model.h`) and the key store port, served through the
shared schema generator and query compiler, `defiant-storage-sql`. This
library will be the Postgres dialect and a libpq executor: per-tenant schemas,
schema-qualified statements, a connection pool that carries no tenant state,
prepared statements per tenant, and index builds with `CREATE INDEX
CONCURRENTLY` outside the step that declares them.

## Building

It depends, through pkg-config, on `defiant-storage`, `defiant-storage-sql`
and `libpq`. libpq is built against only inside the image the `Containerfile`
here defines (`ghoti-build:gcc16` plus `libpq-dev`), never on a developer host;
without it the Makefile stops and says so.

## Status

The repository's scaffold: result codes and the allocator. The backend
itself comes next.
