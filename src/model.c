/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Defiant Storage Postgres.
 *
 * Ghoti.io Defiant Storage Postgres is free software: you can redistribute it
 * and/or modify it under the terms of the GNU Lesser General Public License
 * version 3 as published by the Free Software Foundation.
 *
 * Ghoti.io Defiant Storage Postgres is distributed in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * The entity port and the key store port on Postgres (model.h): the dialect
 * defiant-storage-sql needs, and the libpq executor that runs its statements.
 * Every statement comes from that library. The only SQL text here is the
 * dialect's: its transaction statements, its index-build words and its two
 * catalog queries (an index's validity, a store's existence). The session
 * settings are libpq connection options, not statements.
 *
 * A pool of connections serves every tenant of a store: the tenant is in
 * every statement's text (its schema), never in a connection's state
 * (AD-14). Each connection keeps its prepared statements keyed by the tenant
 * and the text. A connection that saw an error is closed, never pooled.
 */

#include <ghoti.io/defiant-storage-postgres/defiant-storage-postgres.h>
#include <ghoti.io/defiant-storage-sql/defiant-storage-sql.h>
#include <ghoti.io/defiant-storage/model.h>
#include <ghoti.io/cutil/allocator.h>
#include <libpq-fe.h>

#include "wire_internal.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the dialect ------------------------------------------------------------ */

static const char kBeginRead[] =
    "BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY";
static const char kBeginWrite[] = "BEGIN ISOLATION LEVEL SERIALIZABLE";
static const char kCommit[] = "COMMIT";
static const char kRollback[] = "ROLLBACK";
/* (schema, index) -> 1 valid, 0 invalid, no row: none. */
static const char kIndexValid[] =
    "SELECT CASE WHEN i.indisvalid THEN 1 ELSE 0 END FROM pg_catalog.pg_index i"
    " JOIN pg_catalog.pg_class c ON c.oid = i.indexrelid"
    " JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace"
    " WHERE n.nspname::text = $1::text AND c.relname::text = $2::text";
/* (schema, version table) -> no row: no schema; else (has it, tables). */
static const char kStoreExists[] =
    "SELECT (SELECT count(*) FROM pg_catalog.pg_class c"
    " WHERE c.relnamespace = n.oid AND c.relkind = 'r'"
    " AND c.relname::text = $2::text),"
    " (SELECT count(*) FROM pg_catalog.pg_class c"
    " WHERE c.relnamespace = n.oid AND c.relkind = 'r')"
    " FROM pg_catalog.pg_namespace n WHERE n.nspname::text = $1::text";

/* Postgres: BIGINT holds every int64 and DOUBLE PRECISION every double
 * exactly; booleans are SMALLINT, bound as an int64 into an untyped
 * parameter; text compares by bytes only under COLLATE "C"; NULL sorts high
 * ascending; DDL is transactional except CREATE INDEX CONCURRENTLY, which the
 * SQL layer runs outside any transaction (AD-15). */
static void fill_dialect(GDEFSSQL_Dialect * d) {
  memset(d, 0, sizeof *d);
  d->size = sizeof *d;
  d->name = "postgres";
  d->quote_open = "\"";
  d->quote_close = "\"";
  d->placeholder = "$";
  d->placeholder_numbered = 1;
  d->type_names[GDEFSSQL_COL_ID] = "BIGINT";
  d->type_names[GDEFSSQL_COL_INTEGER] = "BIGINT";
  d->type_names[GDEFSSQL_COL_DECIMAL] = "BIGINT";
  d->type_names[GDEFSSQL_COL_FLOAT] = "DOUBLE PRECISION";
  d->type_names[GDEFSSQL_COL_BOOLEAN] = "SMALLINT";
  d->type_names[GDEFSSQL_COL_DATE] = "BIGINT";
  d->type_names[GDEFSSQL_COL_DATETIME] = "BIGINT";
  d->type_names[GDEFSSQL_COL_TEXT] = "TEXT";
  d->type_names[GDEFSSQL_COL_REFERENCE] = "TEXT";
  d->type_names[GDEFSSQL_COL_BYTES] = "BYTEA";
  /* Byte order for every text column, whatever the database's collation. */
  d->text_collation = "COLLATE \"C\"";
  d->upsert = GDEFSSQL_UPSERT_ON_CONFLICT;
  d->returning = 1;
  d->nulls_low = 0;
  d->ddl_transactional = 1;
  d->max_identifier = 63;   /* NAMEDATALEN - 1 */
  d->max_binds = 65535;     /* the protocol's parameter count is 16 bits */
  d->max_columns = 1600;    /* MaxHeapAttributeNumber */
  d->begin_read = kBeginRead;
  d->begin_write = kBeginWrite;
  d->commit = kCommit;
  d->rollback = kRollback;
  d->index_build = "CONCURRENTLY ";
  d->index_valid = kIndexValid;
  d->index_drop = "DROP INDEX CONCURRENTLY IF EXISTS ";
  d->qualifier_create = "CREATE SCHEMA IF NOT EXISTS ";
  d->store_exists = kStoreExists;
  /* A deleted key row is a dead tuple until a vacuum; FULL writes the table
   * anew, so the old pages go with it (dialect.h, key_vacuum). */
  d->key_vacuum = "VACUUM (FULL) ";
}

void gdefspg_dialect(GDEFSSQL_Dialect * out) {
  if (out != NULL) {
    fill_dialect(out);
  }
}

void gdefspg_options_default(GDEFSPG_Options * out) {
  if (out == NULL) {
    return;
  }
  memset(out, 0, sizeof *out);
  out->size = sizeof *out;
  out->lock_timeout_ms = 5000;
  out->idle_connections = 8;
  out->statement_cache = 64;
}

GDEFSPG_Result gdefspg_schema_name(const char * prefix, const char * tenant,
    size_t tenant_len, char * out, size_t out_cap) {
  char name[64];
  int rc;
  if (out == NULL || out_cap < 64) {
    return GDEFSPG_ERR_INVALID;
  }
  rc = gdefspg_wire_schema(prefix != NULL ? prefix : "ds_", tenant,
      tenant_len, name);
  if (rc == -2) {
    return GDEFSPG_ERR_LIMIT;
  }
  if (rc != 0) {
    return GDEFSPG_ERR_INVALID;
  }
  memcpy(out, name, strlen(name) + 1);
  return GDEFSPG_OK;
}

/* ---- the executor's state --------------------------------------------------- */

struct XConn;

typedef struct XParam {
  char num[24];         /* an INT, as text */
  uint8_t real[8];      /* a REAL, as float8 */
} XParam;

typedef struct XStmt {
  struct XConn * conn;
  char name[24];        /* the server's name for it; "" for the unnamed one */
  char * sql;           /* the cache key, with the tenant below */
  size_t len;
  char * tenant;
  size_t tenant_len;
  int kept;             /* in the connection's cache */
  int in_use;
  int cached;           /* this use came from the cache */
  uint64_t used;
  /* one use */
  GDEFSSQL_Datum * binds;
  XParam * params;
  size_t nbinds;
  size_t capbinds;
  PGresult * res;
  int ran;
  int failed;
  int in_txn_at_run;    /* a transaction was open when it ran */
  int row;              /* the current row, 0-based; -1 before the first */
  uint64_t rows;
} XStmt;

typedef struct XConn {
  PGconn * pg;
  struct Ex * ex;
  char * tenant;        /* the tenant it is open for, while in use */
  size_t tenant_len;
  char schema[64];
  int write;
  int bad;              /* an error was seen: never pooled again */
  XStmt ** cache;
  size_t ncache;
  unsigned next_name;
  struct XConn * next;  /* in the idle list */
} XConn;

/* A tenant whose store this executor has checked, so its schema is known to
 * hold this layout's version. */
typedef struct XKnown {
  char * tenant;
  size_t len;
  struct XKnown * next;
} XKnown;

typedef struct Ex {
  const GDEFSPG_Allocator * alloc;
  int keys;
  GDEFSPG_Options opts;
  char * conninfo;
  char * prefix;
  char * session;       /* the libpq `options` every connection is made with */
  GDEFSSQL_Dialect d;
  GDEFSSQL_Executor self;
  XConn * idle;
  size_t nidle;
  XKnown * known;
  uint64_t tick;
} Ex;

static char * dup_bytes(const GDEFSPG_Allocator * a, const char * p, size_t n) {
  char * c = (char *)gcu_allocator_malloc(a, n + 1);
  if (c == NULL) {
    return NULL;
  }
  if (n > 0) {
    memcpy(c, p, n);
  }
  c[n] = '\0';
  return c;
}

static GDEFSPG_Result to_lib(GDEFSTOR_Result rc) {
  switch (rc) {
  case GDEFSTOR_OK:
    return GDEFSPG_OK;
  case GDEFSTOR_ERR_IO:
    return GDEFSPG_ERR_IO;
  case GDEFSTOR_ERR_FORMAT:
    return GDEFSPG_ERR_FORMAT;
  case GDEFSTOR_ERR_UNSUPPORTED:
    return GDEFSPG_ERR_UNSUPPORTED;
  case GDEFSTOR_ERR_LIMIT:
    return GDEFSPG_ERR_LIMIT;
  case GDEFSTOR_ERR_CORRUPT:
    return GDEFSPG_ERR_CORRUPT;
  case GDEFSTOR_ERR_OOM:
    return GDEFSPG_ERR_OOM;
  case GDEFSTOR_ERR_INVALID:
    return GDEFSPG_ERR_INVALID;
  case GDEFSTOR_ERR_BUSY:
    return GDEFSPG_ERR_BUSY;
  default:
    return GDEFSPG_ERR_INTERNAL;
  }
}

static GDEFSPG_Result from_sql(GDEFSSQL_Result rc) {
  return to_lib(gdefssql_result_to_port(rc));
}

/* What a failed statement means to the port. A conflict the server resolved
 * by giving up (a serialization failure, a deadlock, a lock or statement
 * timeout, a cancel) can be retried; anything else is an I/O error. */
static GDEFSTOR_Result from_result(const PGresult * res) {
  const char * state = res != NULL
                           ? PQresultErrorField(res, PG_DIAG_SQLSTATE)
                           : NULL;
  if (state == NULL) {
    return GDEFSTOR_ERR_IO;
  }
  if (strcmp(state, "40001") == 0 || strcmp(state, "40P01") == 0 ||
      strcmp(state, "55P03") == 0 || strcmp(state, "57014") == 0) {
    return GDEFSTOR_ERR_BUSY;
  }
  if (strcmp(state, "53200") == 0) {
    return GDEFSTOR_ERR_OOM;
  }
  if (strcmp(state, "54000") == 0 || strcmp(state, "54001") == 0 ||
      strcmp(state, "54011") == 0 || strcmp(state, "54023") == 0) {
    return GDEFSTOR_ERR_LIMIT;
  }
  return GDEFSTOR_ERR_IO;
}

/* ---- statements: reporting ---------------------------------------------------- */

static void report(Ex * ex, XConn * c, const char * sql, size_t len, int exec,
    int in_txn, int cached, int failed, uint64_t rows,
    const GDEFSSQL_Datum * binds, size_t nbinds) {
  GDEFSPG_StatementStats s;
  if (ex->opts.on_statement == NULL) {
    return;
  }
  memset(&s, 0, sizeof s);
  s.size = sizeof s;
  s.sql = sql;
  s.sql_len = len;
  s.tenant = c->tenant != NULL ? c->tenant : "";
  s.tenant_len = c->tenant_len;
  s.backend_pid = PQbackendPID(c->pg);
  s.write = c->write;
  s.keys = ex->keys;
  s.exec = exec;
  s.in_transaction = in_txn;
  s.cached = cached;
  s.failed = failed;
  s.rows = rows;
  s.binds = binds;
  s.bind_count = nbinds;
  ex->opts.on_statement(ex->opts.on_statement_ctx, &s);
}

static int in_transaction(PGconn * pg) {
  PGTransactionStatusType t = PQtransactionStatus(pg);
  return t == PQTRANS_INTRANS || t == PQTRANS_INERROR ||
         t == PQTRANS_ACTIVE;
}

/* ---- connections ------------------------------------------------------------ */

static void stmt_free(Ex * ex, XStmt * s) {
  if (s->res != NULL) {
    PQclear(s->res);
  }
  gcu_allocator_free(ex->alloc, s->binds);
  gcu_allocator_free(ex->alloc, s->params);
  gcu_allocator_free(ex->alloc, s->sql);
  gcu_allocator_free(ex->alloc, s->tenant);
  gcu_allocator_free(ex->alloc, s);
}

static void conn_close(Ex * ex, XConn * c) {
  size_t i;
  for (i = 0; i < c->ncache; i++) {
    stmt_free(ex, c->cache[i]);
  }
  gcu_allocator_free(ex->alloc, c->cache);
  gcu_allocator_free(ex->alloc, c->tenant);
  if (c->pg != NULL) {
    PQfinish(c->pg);
  }
  gcu_allocator_free(ex->alloc, c);
}

/* A new connection: the caller's conninfo, with its own options kept and the
 * session settings added (libpq's `options`, applied by the server at
 * connect, the same on every connection, so nothing in them is a tenant's). */
static GDEFSTOR_Result conn_new(Ex * ex, XConn ** out) {
  const char * keys[8];
  const char * vals[8];
  char * merged = NULL;
  PQconninfoOption * parsed = NULL;
  PQconninfoOption * o;
  char * err = NULL;
  const char * have = NULL;
  XConn * c;
  size_t k = 0;
  *out = NULL;
  parsed = PQconninfoParse(ex->conninfo, &err);
  if (parsed == NULL) {
    PQfreemem(err);
    return GDEFSTOR_ERR_INVALID;
  }
  for (o = parsed; o->keyword != NULL; o++) {
    if (strcmp(o->keyword, "options") == 0 && o->val != NULL) {
      have = o->val;
    }
  }
  if (have != NULL && have[0] != '\0') {
    size_t a = strlen(have);
    size_t b = strlen(ex->session);
    merged = (char *)gcu_allocator_malloc(ex->alloc, a + b + 2);
    if (merged == NULL) {
      PQconninfoFree(parsed);
      return GDEFSTOR_ERR_OOM;
    }
    memcpy(merged, have, a);
    merged[a] = ' ';
    memcpy(merged + a + 1, ex->session, b + 1);
  }
  PQconninfoFree(parsed);
  /* dbname first, expanded as a connection string; what follows overrides
   * what it says. */
  keys[k] = "dbname";
  vals[k++] = ex->conninfo;
  keys[k] = "options";
  vals[k++] = merged != NULL ? merged : ex->session;
  keys[k] = "client_encoding";
  vals[k++] = "UTF8";
  keys[k] = "application_name";
  vals[k++] = ex->keys ? "defiant-storage-postgres keys"
                       : "defiant-storage-postgres";
  keys[k] = NULL;
  vals[k] = NULL;
  c = (XConn *)gcu_allocator_calloc(ex->alloc, 1, sizeof *c);
  if (c == NULL) {
    gcu_allocator_free(ex->alloc, merged);
    return GDEFSTOR_ERR_OOM;
  }
  c->ex = ex;
  c->pg = PQconnectdbParams(keys, vals, 1);
  gcu_allocator_free(ex->alloc, merged);
  if (c->pg == NULL) {
    gcu_allocator_free(ex->alloc, c);
    return GDEFSTOR_ERR_OOM;
  }
  if (PQstatus(c->pg) != CONNECTION_OK) {
    conn_close(ex, c);
    return GDEFSTOR_ERR_IO;
  }
  /* PQclosePrepared (libpq and server 17) evicts a cached statement. */
  if (PQserverVersion(c->pg) < 170000) {
    conn_close(ex, c);
    return GDEFSTOR_ERR_UNSUPPORTED;
  }
  *out = c;
  return GDEFSTOR_OK;
}

static XKnown * known_find(Ex * ex, const char * t, size_t n) {
  XKnown * k;
  for (k = ex->known; k != NULL; k = k->next) {
    if (k->len == n && (n == 0 || memcmp(k->tenant, t, n) == 0)) {
      return k;
    }
  }
  return NULL;
}

static void known_add(Ex * ex, const char * t, size_t n) {
  XKnown * k = (XKnown *)gcu_allocator_calloc(ex->alloc, 1, sizeof *k);
  if (k == NULL) {
    return; /* not remembering it costs only checking again */
  }
  k->tenant = dup_bytes(ex->alloc, t, n);
  if (k->tenant == NULL) {
    gcu_allocator_free(ex->alloc, k);
    return;
  }
  k->len = n;
  k->next = ex->known;
  ex->known = k;
}

static void refuse(Ex * ex, GDEFSTOR_Result rc, const char * schema,
    const char * cause) {
  char line[512];
  int n = snprintf(line, sizeof line,
      "defiant-storage-postgres: schema %s %s; it is refused and left as it is",
      schema, cause);
  size_t len;
  if (n < 0) {
    return;
  }
  len = (size_t)n < sizeof line ? (size_t)n : sizeof line - 1;
  if (ex->opts.on_refusal != NULL) {
    ex->opts.on_refusal(ex->opts.on_refusal_ctx, (int)rc, line, len);
  } else {
    fprintf(stderr, "%s\n", line);
  }
}

/* Give a connection back: to the pool, or closed. */
static void conn_release(Ex * ex, XConn * c, int discard) {
  gcu_allocator_free(ex->alloc, c->tenant);
  c->tenant = NULL;
  c->tenant_len = 0;
  c->schema[0] = '\0';
  /* After an error, mid-transaction, or broken: never handed out again
   * (AD-14). */
  if (discard || c->bad || PQstatus(c->pg) != CONNECTION_OK ||
      PQtransactionStatus(c->pg) != PQTRANS_IDLE ||
      ex->nidle >= ex->opts.idle_connections) {
    conn_close(ex, c);
    return;
  }
  if (ex->opts.on_idle != NULL) {
    ex->opts.on_idle(ex->opts.on_idle_ctx, c->pg);
  }
  c->next = ex->idle;
  ex->idle = c;
  ex->nidle += 1;
}

/* Is this tenant's store there and this layout? Run once per tenant per
 * executor, before a connection is handed out for it. */
static GDEFSTOR_Result check_store(Ex * ex, XConn * c, int * out_exists) {
  int kind = ex->keys ? GDEFSSQL_STORE_KEYS : GDEFSSQL_STORE_DATA;
  int64_t want = ex->keys ? GDEFSSQL_KEYSTORE_VERSION : GDEFSSQL_SCHEMA_VERSION;
  int64_t version = 0;
  int exists = 0;
  GDEFSTOR_Result rc = gdefssql_store_exists(&ex->d, &ex->self, c, c->schema,
      kind, &exists);
  *out_exists = 0;
  if (rc == GDEFSTOR_ERR_FORMAT) {
    refuse(ex, rc, c->schema,
        ex->keys ? "holds tables but no dsk_schema (not a key store)"
                 : "holds tables but no dss_schema (not a data store)");
    return rc;
  }
  if (rc != GDEFSTOR_OK || !exists) {
    return rc;
  }
  rc = gdefssql_store_version(&ex->d, &ex->self, c, c->schema, kind, &version);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (version != want) {
    char cause[96];
    GDEFSTOR_Result why =
        version > want ? GDEFSTOR_ERR_UNSUPPORTED : GDEFSTOR_ERR_FORMAT;
    snprintf(cause, sizeof cause,
        "records layout version %lld; this build reads version %lld",
        (long long)version, (long long)want);
    refuse(ex, why, c->schema, cause);
    return why;
  }
  *out_exists = 1;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result x_open(void * ctx, const char * tenant,
    size_t tenant_len, int write, void ** out_conn, const char ** out_qual) {
  Ex * ex = (Ex *)ctx;
  XConn * c = NULL;
  char schema[64];
  int named;
  GDEFSTOR_Result rc;
  *out_conn = NULL;
  *out_qual = NULL;
  if (tenant == NULL || tenant_len == 0) {
    return GDEFSTOR_ERR_INVALID;
  }
  named = gdefspg_wire_schema(ex->prefix, tenant, tenant_len, schema);
  if (named != 0) {
    return named == -2 ? GDEFSTOR_ERR_LIMIT : GDEFSTOR_ERR_INTERNAL;
  }
  if (ex->idle != NULL) {
    c = ex->idle;
    ex->idle = c->next;
    ex->nidle -= 1;
    c->next = NULL;
  } else {
    rc = conn_new(ex, &c);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  c->tenant = dup_bytes(ex->alloc, tenant, tenant_len);
  if (c->tenant == NULL) {
    conn_close(ex, c);
    return GDEFSTOR_ERR_OOM;
  }
  c->tenant_len = tenant_len;
  c->write = write;
  memcpy(c->schema, schema, strlen(schema) + 1);
  if (known_find(ex, tenant, tenant_len) == NULL) {
    int exists = 0;
    rc = check_store(ex, c, &exists);
    if (rc != GDEFSTOR_OK) {
      conn_release(ex, c, c->bad);
      return rc;
    }
    if (exists) {
      known_add(ex, tenant, tenant_len);
    } else if (!write) {
      /* No store: a read of it holds nothing (executor.h). */
      conn_release(ex, c, 0);
      return GDEFSTOR_OK;
    }
    /* A write makes the store; the next open checks it. */
  }
  *out_conn = c;
  *out_qual = c->schema;
  return GDEFSTOR_OK;
}

static void x_close(void * ctx, void * conn, int discard) {
  Ex * ex = (Ex *)ctx;
  XConn * c = (XConn *)conn;
  if (c == NULL) {
    return;
  }
  conn_release(ex, c, discard);
}

/* A statement's text with its NUL, for libpq, which reads up to one. */
static char * terminated(Ex * ex, const char * sql, size_t len) {
  if (len >= (size_t)INT_MAX) {
    return NULL;
  }
  return dup_bytes(ex->alloc, sql, len);
}

static GDEFSTOR_Result x_exec(void * ctx, void * conn, const char * sql,
    size_t sql_len) {
  Ex * ex = (Ex *)ctx;
  XConn * c = (XConn *)conn;
  char * text = terminated(ex, sql, sql_len);
  PGresult * res;
  GDEFSTOR_Result rc = GDEFSTOR_OK;
  int in_txn;
  if (text == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  in_txn = in_transaction(c->pg);
  res = PQexec(c->pg, text);
  if (res == NULL) {
    rc = GDEFSTOR_ERR_OOM;
  } else if (PQresultStatus(res) != PGRES_COMMAND_OK &&
             PQresultStatus(res) != PGRES_TUPLES_OK) {
    rc = from_result(res);
  }
  if (rc != GDEFSTOR_OK) {
    c->bad = 1;
  }
  report(ex, c, sql, sql_len, 1, in_txn, 0, rc != GDEFSTOR_OK, 0, NULL, 0);
  if (res != NULL) {
    PQclear(res);
  }
  gcu_allocator_free(ex->alloc, text);
  return rc;
}

/* Make room in the cache for one more: evict the least recently used idle
 * statement, closing it on the server. */
static void cache_evict(Ex * ex, XConn * c) {
  size_t lru = SIZE_MAX;
  size_t i;
  for (i = 0; i < c->ncache; i++) {
    if (!c->cache[i]->in_use &&
        (lru == SIZE_MAX || c->cache[i]->used < c->cache[lru]->used)) {
      lru = i;
    }
  }
  if (lru == SIZE_MAX) {
    return;
  }
  {
    XStmt * s = c->cache[lru];
    PGresult * r = PQclosePrepared(c->pg, s->name);
    if (r == NULL || PQresultStatus(r) != PGRES_COMMAND_OK) {
      c->bad = 1; /* the server's state is no longer what the cache says */
    }
    if (r != NULL) {
      PQclear(r);
    }
    stmt_free(ex, s);
    c->cache[lru] = c->cache[c->ncache - 1];
    c->ncache -= 1;
  }
}

static GDEFSTOR_Result x_prepare(void * ctx, void * conn, const char * sql,
    size_t sql_len, void ** out_stmt) {
  Ex * ex = (Ex *)ctx;
  XConn * c = (XConn *)conn;
  XStmt * s = NULL;
  PGresult * r;
  size_t i;
  int keep = ex->opts.statement_cache > 0;
  *out_stmt = NULL;
  if (sql_len >= (size_t)INT_MAX) {
    return GDEFSTOR_ERR_LIMIT;
  }
  /* Keyed by the tenant and the text (AD-14). */
  for (i = 0; i < c->ncache; i++) {
    XStmt * k = c->cache[i];
    if (!k->in_use && k->len == sql_len && memcmp(k->sql, sql, sql_len) == 0 &&
        k->tenant_len == c->tenant_len &&
        memcmp(k->tenant, c->tenant, c->tenant_len) == 0) {
      s = k;
      s->cached = 1;
      break;
    }
  }
  if (s == NULL) {
    s = (XStmt *)gcu_allocator_calloc(ex->alloc, 1, sizeof *s);
    if (s == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    s->conn = c;
    s->sql = terminated(ex, sql, sql_len);
    s->tenant = dup_bytes(ex->alloc, c->tenant, c->tenant_len);
    if (s->sql == NULL || s->tenant == NULL) {
      stmt_free(ex, s);
      return GDEFSTOR_ERR_OOM;
    }
    s->len = sql_len;
    s->tenant_len = c->tenant_len;
    if (keep) {
      if (c->ncache == ex->opts.statement_cache) {
        cache_evict(ex, c);
      }
      if (c->cache == NULL) {
        c->cache = (XStmt **)gcu_allocator_calloc(ex->alloc,
            ex->opts.statement_cache, sizeof *c->cache);
      }
      keep = c->cache != NULL && c->ncache < ex->opts.statement_cache;
    }
    if (keep) {
      snprintf(s->name, sizeof s->name, "s%u", ++c->next_name);
    }
    /* Untyped parameters: the server types each from where it stands. */
    r = PQprepare(c->pg, s->name, s->sql, 0, NULL);
    if (r == NULL || PQresultStatus(r) != PGRES_COMMAND_OK) {
      GDEFSTOR_Result rc = r == NULL ? GDEFSTOR_ERR_OOM : from_result(r);
      if (r != NULL) {
        PQclear(r);
      }
      c->bad = 1;
      report(ex, c, sql, sql_len, 0, in_transaction(c->pg), 0, 1, 0, NULL, 0);
      stmt_free(ex, s);
      return rc;
    }
    PQclear(r);
    if (keep) {
      c->cache[c->ncache++] = s;
      s->kept = 1;
    }
  }
  if (s->binds != NULL) {
    /* Every position is bound again for this use; one that is not reads as
     * never bound. */
    memset(s->binds, 0, s->capbinds * sizeof *s->binds);
  }
  s->in_use = 1;
  s->ran = 0;
  s->failed = 0;
  s->row = -1;
  s->rows = 0;
  s->nbinds = 0;
  *out_stmt = s;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result x_bind(void * ctx, void * stmt, unsigned index,
    const GDEFSSQL_Datum * v) {
  Ex * ex = (Ex *)ctx;
  XStmt * s = (XStmt *)stmt;
  size_t at;
  if (index == 0 || index > 65535 || v == NULL || s->ran ||
      v->kind == GDEFSSQL_DATUM_NULL || v->kind > GDEFSSQL_DATUM_BYTES) {
    return GDEFSTOR_ERR_INTERNAL;
  }
  at = index - 1;
  if (at >= s->capbinds) {
    size_t cap = s->capbinds == 0 ? 16 : s->capbinds;
    GDEFSSQL_Datum * b;
    XParam * p;
    while (cap <= at) {
      cap *= 2;
    }
    b = (GDEFSSQL_Datum *)gcu_allocator_realloc(ex->alloc, s->binds,
        cap * sizeof *b);
    if (b == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    s->binds = b;
    p = (XParam *)gcu_allocator_realloc(ex->alloc, s->params, cap * sizeof *p);
    if (p == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    s->params = p;
    memset(b + s->capbinds, 0, (cap - s->capbinds) * sizeof *b);
    s->capbinds = cap;
  }
  /* The SQL layer keeps the bytes alive until it finalizes (executor.h). */
  s->binds[at] = *v;
  if (v->kind == GDEFSSQL_DATUM_INT) {
    gdefspg_wire_int_text(v->integer, s->params[at].num);
  } else if (v->kind == GDEFSSQL_DATUM_REAL) {
    gdefspg_wire_float8(v->real, s->params[at].real);
  }
  if (index > s->nbinds) {
    s->nbinds = index;
  }
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result run(Ex * ex, XStmt * s) {
  const char * vals_s[16];
  int lens_s[16];
  int fmts_s[16];
  const char ** vals = vals_s;
  int * lens = lens_s;
  int * fmts = fmts_s;
  size_t i;
  GDEFSTOR_Result rc = GDEFSTOR_OK;
  ExecStatusType st;
  if (s->nbinds > 16) {
    vals = (const char **)gcu_allocator_malloc(ex->alloc,
        s->nbinds * sizeof *vals);
    lens = (int *)gcu_allocator_malloc(ex->alloc, s->nbinds * sizeof *lens);
    fmts = (int *)gcu_allocator_malloc(ex->alloc, s->nbinds * sizeof *fmts);
    if (vals == NULL || lens == NULL || fmts == NULL) {
      rc = GDEFSTOR_ERR_OOM;
    }
  }
  for (i = 0; rc == GDEFSTOR_OK && i < s->nbinds; i++) {
    const GDEFSSQL_Datum * d = &s->binds[i];
    switch (d->kind) {
    case GDEFSSQL_DATUM_INT:
      vals[i] = s->params[i].num;
      lens[i] = 0;
      fmts[i] = 0;
      break;
    case GDEFSSQL_DATUM_REAL:
      vals[i] = (const char *)s->params[i].real;
      lens[i] = 8;
      fmts[i] = 1;
      break;
    case GDEFSSQL_DATUM_TEXT:
    case GDEFSSQL_DATUM_BYTES:
      if (d->len > (size_t)INT_MAX) {
        rc = GDEFSTOR_ERR_LIMIT;
        break;
      }
      vals[i] = d->len == 0 ? "" : (const char *)d->bytes;
      lens[i] = (int)d->len;
      fmts[i] = 1;
      break;
    default:
      /* A position the SQL layer never bound. */
      rc = GDEFSTOR_ERR_INTERNAL;
      break;
    }
  }
  if (rc == GDEFSTOR_OK) {
    s->res = PQexecPrepared(s->conn->pg, s->name, (int)s->nbinds, vals, lens,
        fmts, 1);
    if (s->res == NULL) {
      rc = GDEFSTOR_ERR_OOM;
    } else {
      st = PQresultStatus(s->res);
      if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK) {
        rc = from_result(s->res);
      }
    }
  }
  if (vals != vals_s) {
    gcu_allocator_free(ex->alloc, (void *)vals);
    gcu_allocator_free(ex->alloc, lens);
    gcu_allocator_free(ex->alloc, fmts);
  }
  return rc;
}

static GDEFSTOR_Result x_step(void * ctx, void * stmt, int * out_row) {
  Ex * ex = (Ex *)ctx;
  XStmt * s = (XStmt *)stmt;
  *out_row = 0;
  if (s->failed) {
    return GDEFSTOR_ERR_IO;
  }
  if (!s->ran) {
    GDEFSTOR_Result rc;
    s->ran = 1;
    s->in_txn_at_run = in_transaction(s->conn->pg);
    rc = run(ex, s);
    if (rc != GDEFSTOR_OK) {
      s->failed = 1;
      s->conn->bad = 1;
      return rc;
    }
  }
  if (s->res != NULL && s->row + 1 < PQntuples(s->res)) {
    s->row += 1;
    s->rows += 1;
    *out_row = 1;
  }
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result x_column(void * ctx, void * stmt, unsigned index,
    int kind, GDEFSSQL_Datum * out) {
  XStmt * s = (XStmt *)stmt;
  const uint8_t * p;
  size_t len;
  unsigned oid;
  (void)ctx;
  memset(out, 0, sizeof *out);
  if (s->res == NULL || s->row < 0 || index >= (unsigned)PQnfields(s->res)) {
    return GDEFSTOR_ERR_INTERNAL;
  }
  if (PQgetisnull(s->res, s->row, (int)index)) {
    out->kind = GDEFSSQL_DATUM_NULL;
    return GDEFSTOR_OK;
  }
  if (PQfformat(s->res, (int)index) != 1) {
    return GDEFSTOR_ERR_CORRUPT;
  }
  p = (const uint8_t *)PQgetvalue(s->res, s->row, (int)index);
  len = (size_t)PQgetlength(s->res, s->row, (int)index);
  oid = (unsigned)PQftype(s->res, (int)index);
  switch (kind) {
  case GDEFSSQL_DATUM_INT:
    if (gdefspg_wire_read_int(oid, p, len, &out->integer) != 0) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    break;
  case GDEFSSQL_DATUM_REAL:
    if (gdefspg_wire_read_float8(oid, p, len, &out->real) != 0) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    break;
  case GDEFSSQL_DATUM_TEXT:
    if (oid != GDEFSPG_OID_TEXT && oid != GDEFSPG_OID_VARCHAR &&
        oid != GDEFSPG_OID_NAME) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    out->bytes = p;
    out->len = len;
    break;
  case GDEFSSQL_DATUM_BYTES:
    if (oid != GDEFSPG_OID_BYTEA) {
      return GDEFSTOR_ERR_CORRUPT;
    }
    out->bytes = p;
    out->len = len;
    break;
  default:
    return GDEFSTOR_ERR_INTERNAL;
  }
  out->kind = kind;
  return GDEFSTOR_OK;
}

static void x_finalize(void * ctx, void * stmt) {
  Ex * ex = (Ex *)ctx;
  XStmt * s = (XStmt *)stmt;
  if (s == NULL) {
    return;
  }
  report(ex, s->conn, s->sql, s->len, 0, s->in_txn_at_run, s->cached,
      s->failed, s->rows, s->binds, s->nbinds);
  if (s->res != NULL) {
    PQclear(s->res);
    s->res = NULL;
  }
  s->in_use = 0;
  s->cached = 0;
  s->nbinds = 0;
  if (s->kept) {
    s->used = ++ex->tick;
  } else {
    stmt_free(ex, s);
  }
}

static void ex_destroy(void * ctx) {
  Ex * ex = (Ex *)ctx;
  if (ex == NULL) {
    return;
  }
  while (ex->idle != NULL) {
    XConn * c = ex->idle;
    ex->idle = c->next;
    conn_close(ex, c);
  }
  while (ex->known != NULL) {
    XKnown * k = ex->known;
    ex->known = k->next;
    gcu_allocator_free(ex->alloc, k->tenant);
    gcu_allocator_free(ex->alloc, k);
  }
  gcu_allocator_free(ex->alloc, ex->conninfo);
  gcu_allocator_free(ex->alloc, ex->prefix);
  gcu_allocator_free(ex->alloc, ex->session);
  gcu_allocator_free(ex->alloc, ex);
}

static GDEFSPG_Result ex_create(const GDEFSPG_Options * options, int keys,
    const GDEFSPG_Allocator * allocator, Ex ** out) {
  const GDEFSPG_Allocator * alloc =
      allocator != NULL ? allocator : gdefspg_allocator_default();
  const char * prefix;
  char probe[64];
  char session[64];
  Ex * ex;
  *out = NULL;
  if (options == NULL || options->size != sizeof *options ||
      options->conninfo == NULL || options->lock_timeout_ms == 0) {
    return GDEFSPG_ERR_INVALID;
  }
  prefix = options->schema_prefix != NULL ? options->schema_prefix
                                          : keys ? "dk_" : "ds_";
  if (gdefspg_wire_schema(prefix, "", 0, probe) != 0) {
    return GDEFSPG_ERR_INVALID;
  }
  /* Every connection's session settings, applied by the server at connect:
   * lock_timeout bounds every wait for a lock (AD-15), the index builds'
   * included; nothing here differs between tenants. */
  snprintf(session, sizeof session, "-c lock_timeout=%u",
      options->lock_timeout_ms);
  ex = (Ex *)gcu_allocator_calloc(alloc, 1, sizeof *ex);
  if (ex == NULL) {
    return GDEFSPG_ERR_OOM;
  }
  ex->alloc = alloc;
  ex->keys = keys;
  ex->opts = *options;
  ex->conninfo = dup_bytes(alloc, options->conninfo, strlen(options->conninfo));
  ex->prefix = dup_bytes(alloc, prefix, strlen(prefix));
  ex->session = dup_bytes(alloc, session, strlen(session));
  if (ex->conninfo == NULL || ex->prefix == NULL || ex->session == NULL) {
    ex_destroy(ex);
    return GDEFSPG_ERR_OOM;
  }
  ex->opts.conninfo = ex->conninfo;
  ex->opts.schema_prefix = ex->prefix;
  fill_dialect(&ex->d);
  memset(&ex->self, 0, sizeof ex->self);
  ex->self.size = sizeof ex->self;
  ex->self.ctx = ex;
  ex->self.destroy = ex_destroy;
  ex->self.open = x_open;
  ex->self.close = x_close;
  ex->self.exec = x_exec;
  ex->self.prepare = x_prepare;
  ex->self.bind = x_bind;
  ex->self.step = x_step;
  ex->self.column = x_column;
  ex->self.finalize = x_finalize;
  *out = ex;
  return GDEFSPG_OK;
}

/* ---- the stores ------------------------------------------------------------- */

GDEFSPG_Result gdefspg_model_backend(const GDEFSPG_Options * options,
    const GDEFSPG_Allocator * allocator, GDEFSTOR_ModelBackend * out) {
  Ex * ex = NULL;
  GDEFSPG_Result rc;
  if (out == NULL) {
    return GDEFSPG_ERR_INVALID;
  }
  rc = ex_create(options, 0, allocator, &ex);
  if (rc != GDEFSPG_OK) {
    return rc;
  }
  rc = from_sql(gdefssql_model_backend(&ex->d, &ex->self, ex->alloc, out));
  if (rc != GDEFSPG_OK) {
    ex_destroy(ex);
  }
  return rc;
}

GDEFSPG_Result gdefspg_model_store_create(const GDEFSPG_Options * options,
    const GDEFSPG_Allocator * allocator, const GDEFSTOR_Limits * limits,
    GDEFSTOR_Store ** out_store) {
  const GDEFSPG_Allocator * alloc =
      allocator != NULL ? allocator : gdefspg_allocator_default();
  GDEFSTOR_ModelBackend model;
  GDEFSPG_Result rc;
  GDEFSTOR_Result prc;
  if (out_store == NULL) {
    return GDEFSPG_ERR_INVALID;
  }
  rc = gdefspg_model_backend(options, alloc, &model);
  if (rc != GDEFSPG_OK) {
    return rc;
  }
  prc = gdefstor_store_open(&model, alloc, limits, out_store);
  if (prc != GDEFSTOR_OK) {
    model.destroy(model.ctx);
  }
  return to_lib(prc);
}

GDEFSPG_Result gdefspg_keystore_create(const GDEFSPG_Options * options,
    const GDEFSTOR_KeyCipher * cipher, const GDEFSPG_Allocator * allocator,
    GDEFSTOR_KeyStore ** out_keystore) {
  Ex * ex = NULL;
  GDEFSTOR_KeyStoreBackend kb;
  GDEFSPG_Result rc;
  GDEFSTOR_Result prc;
  if (out_keystore == NULL || cipher == NULL) {
    return GDEFSPG_ERR_INVALID;
  }
  rc = ex_create(options, 1, allocator, &ex);
  if (rc != GDEFSPG_OK) {
    return rc;
  }
  rc = from_sql(gdefssql_keystore_backend(&ex->d, &ex->self, cipher, ex->alloc,
      &kb));
  if (rc != GDEFSPG_OK) {
    ex_destroy(ex);
    return rc;
  }
  prc = gdefstor_keystore_create(&kb, ex->alloc, out_keystore);
  if (prc != GDEFSTOR_OK) {
    kb.destroy(kb.ctx);
  }
  return to_lib(prc);
}
