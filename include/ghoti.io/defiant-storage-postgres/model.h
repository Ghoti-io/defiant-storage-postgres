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
 * @file model.h
 * @brief The entity port (defiant-storage's model.h) and the key store port
 * on Postgres, through defiant-storage-sql.
 * @stability free
 *
 * defiant-storage-sql writes every statement; this library gives it the
 * Postgres dialect and a libpq executor, and owns the connections, the
 * prepared statements and the session settings (documentation/postgres.md).
 *
 * **Placement (AD-14).** One database holds every tenant's data store, each
 * in a schema of its own named by GDEFSPG_Options::schema_prefix and the
 * lowercase hex of the tenant bytes; every statement names its tables with
 * that schema. The tenant is never connection state: this library sets no
 * `search_path` and nothing else a connection could carry from one tenant to
 * the next, and any connection serves any tenant. A key store is the same
 * arrangement in a database of its own, never the data store's.
 *
 * **Connections.** A pool of idle connections is shared by every tenant of a
 * store. A connection is made with the conninfo the caller gives and the
 * session's `lock_timeout` (GDEFSPG_Options::lock_timeout_ms), which bounds
 * every wait for a lock, a schema step's included, and the wait of a
 * concurrent index build. A connection that saw an error, or that is
 * returned inside a transaction, is closed, never reused. Prepared
 * statements are kept per connection, keyed by tenant and statement text.
 * Writes run in `SERIALIZABLE` transactions; a read is one `REPEATABLE READ
 * READ ONLY` transaction, so its total and its page are one snapshot. A
 * serialization failure, a deadlock, a lock timeout or a cancelled statement
 * is ::GDEFSTOR_ERR_BUSY: the call can be retried.
 *
 * **What is refused.** The first time a store meets a tenant it reads the
 * schema's layout version (`dss_schema`, or `dsk_schema` for a key store); a
 * schema that holds tables but not that one, or another layout version, is
 * refused and left as it is: ::GDEFSTOR_ERR_UNSUPPORTED for a newer version,
 * ::GDEFSTOR_ERR_FORMAT otherwise, reported through GDEFSPG_Options::on_refusal.
 *
 * **libpq** is found with `pkg-config libpq` and is built against only
 * inside the image the repository's `Containerfile` defines; it is never
 * installed on a developer host.
 *
 * A store or key store made here is used from one thread at a time.
 */

#ifndef GHOTI_IO_GDEFSPG_MODEL_H
#define GHOTI_IO_GDEFSPG_MODEL_H

#include <ghoti.io/defiant-storage-postgres/macros.h>
#include <ghoti.io/defiant-storage-postgres/allocator.h>
#include <ghoti.io/defiant-storage-postgres/core.h>
#include <ghoti.io/defiant-storage-sql/dialect.h>
#include <ghoti.io/defiant-storage-sql/executor.h>
#include <ghoti.io/defiant-storage/keystore.h>
#include <ghoti.io/defiant-storage/model_backend.h>
#include <ghoti.io/defiant-storage/store.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The longest GDEFSPG_Options::schema_prefix, in bytes. */
#define GDEFSPG_MAX_SCHEMA_PREFIX 15

/**
 * @brief What one statement did: for tests and diagnostics, never for
 * timings. Every pointer is borrowed for the call.
 */
typedef struct GDEFSPG_StatementStats {
  size_t size;               ///< `sizeof(GDEFSPG_StatementStats)`.
  const char * sql;          ///< The statement's text.
  size_t sql_len;            ///< Its length.
  const char * tenant;       ///< The tenant the connection was opened for.
  size_t tenant_len;         ///< Its length.
  /** The server process serving the connection (`PQbackendPID`): the same
   * number is the same server connection. */
  int backend_pid;
  int write;                 ///< Non-zero when opened for a tenant's write.
  int keys;                  ///< Non-zero in a key store.
  /** Non-zero for a statement run by the executor's exec: DDL, index builds
   * and the transaction statements. */
  int exec;
  /** Non-zero when a transaction was open on the connection as the statement
   * began. An index build reports zero (AD-15). */
  int in_transaction;
  int cached;                ///< Its prepared form came from the cache.
  int failed;                ///< The server reported an error for it.
  uint64_t rows;             ///< Rows it returned.
  const GDEFSSQL_Datum * binds; ///< Its parameters, in order.
  size_t bind_count;         ///< How many.
} GDEFSPG_StatementStats;

/** Called once for each statement: when an exec returns, and when the SQL
 * layer finalizes a prepared statement. */
typedef void (*GDEFSPG_StatementObserver)(void * ctx,
    const GDEFSPG_StatementStats * stats);

/**
 * Called each time a connection goes back to the pool to be reused, with the
 * libpq connection (a `PGconn *`): for tests and diagnostics, which must leave
 * it as they found it.
 */
typedef void (*GDEFSPG_IdleObserver)(void * ctx, void * pgconn);

/**
 * Called when a tenant's schema is refused. @p result is a ::GDEFSTOR_Result;
 * @p text is one sentence naming the schema and the cause, borrowed.
 */
typedef void (*GDEFSPG_Refusal)(void * ctx, int result, const char * text,
    size_t len);

/** How a store or key store is opened. Its @ref size must be
 * `sizeof(GDEFSPG_Options)`; any other is ::GDEFSPG_ERR_INVALID. */
typedef struct GDEFSPG_Options {
  size_t size;               ///< `sizeof(GDEFSPG_Options)`.
  /** A libpq connection string (`host=... dbname=...` or a URI). Required.
   * Copied. Its own `options`, if any, are kept, and the session settings
   * below are added to them. */
  const char * conninfo;
  /** Written in front of the hex of each tenant to name its schema:
   * `[a-z][a-z0-9_]*`, at most ::GDEFSPG_MAX_SCHEMA_PREFIX bytes, not
   * starting `pg_`. NULL uses "ds_" for a data store and "dk_" for a key
   * store. A tenant whose schema name would pass 63 bytes is
   * ::GDEFSTOR_ERR_LIMIT. Copied. */
  const char * schema_prefix;
  /** `lock_timeout` of every connection, in milliseconds: how long a
   * statement, a schema step or an index build waits for a lock before it
   * fails with ::GDEFSTOR_ERR_BUSY. Default 5000; 0 is ::GDEFSPG_ERR_INVALID
   * (a step must never wait forever, AD-15). */
  unsigned lock_timeout_ms;
  /** Idle connections kept for reuse. Default 8. 0 keeps none. */
  size_t idle_connections;
  /** Prepared statements kept per connection. Default 64. 0 keeps none. */
  size_t statement_cache;
  GDEFSPG_StatementObserver on_statement; ///< May be NULL.
  void * on_statement_ctx;   ///< Passed to @ref on_statement.
  GDEFSPG_IdleObserver on_idle; ///< May be NULL.
  void * on_idle_ctx;        ///< Passed to @ref on_idle.
  /** May be NULL: each refusal is then written as one line to stderr. */
  GDEFSPG_Refusal on_refusal;
  void * on_refusal_ctx;     ///< Passed to @ref on_refusal.
} GDEFSPG_Options;

/** @brief Fill @p out with the defaults (no conninfo). NULL is ignored. */
GDEFSPG_API void gdefspg_options_default(GDEFSPG_Options * out);

/**
 * @brief The Postgres dialect this library hands defiant-storage-sql. Its
 * strings are static. For tests and diagnostics.
 */
GDEFSPG_API void gdefspg_dialect(GDEFSSQL_Dialect * out);

/**
 * @brief The schema a tenant's store lives in, under @p prefix (NULL: the
 * data store's default).
 *
 * @param out Receives the name and a NUL; at least 64 bytes.
 * @return ::GDEFSPG_OK, ::GDEFSPG_ERR_INVALID (a bad prefix or argument), or
 *   ::GDEFSPG_ERR_LIMIT (a tenant too long to name within 63 bytes).
 */
GDEFSPG_API GDEFSPG_Result gdefspg_schema_name(const char * prefix,
    const char * tenant, size_t tenant_len, char * out, size_t out_cap);

/**
 * @brief The entity backend over one database, for
 * gdefstor_store_open() or for a caller that wraps it.
 *
 * Nothing connects yet: the first call that needs a connection makes one.
 *
 * @param options Required (its conninfo is). Copied; the callbacks' contexts
 *   are borrowed for the backend's life.
 * @param allocator NULL uses gdefspg_allocator_default(). It must be the
 *   allocator the store is created with.
 * @param out Receives the backend; GDEFSTOR_ModelBackend::destroy closes
 *   every connection.
 * @return ::GDEFSPG_OK, ::GDEFSPG_ERR_INVALID (a bad option, or an options
 *   size that is not this header's), or ::GDEFSPG_ERR_OOM.
 */
GDEFSPG_API GDEFSPG_Result gdefspg_model_backend(
    const GDEFSPG_Options * options, const GDEFSPG_Allocator * allocator,
    GDEFSTOR_ModelBackend * out);

/**
 * @brief Open a store with the entity port (model.h) over one database.
 *
 * The store is opened with gdefstor_store_open() over
 * gdefspg_model_backend().
 *
 * @param limits NULL uses the port's defaults.
 * @param out_store Receives the store; close it with gdefstor_store_destroy().
 * @return As gdefspg_model_backend(), or the port's failure.
 */
GDEFSPG_API GDEFSPG_Result gdefspg_model_store_create(
    const GDEFSPG_Options * options, const GDEFSPG_Allocator * allocator,
    const GDEFSTOR_Limits * limits, GDEFSTOR_Store ** out_store);

/**
 * @brief Open a key store (defiant-storage's keystore.h) over a database of
 * its own, never a data store's (AD-13, AD-19).
 *
 * What Postgres keeps of a destroyed key (dead tuples until vacuum, the WAL
 * and its archives) is outside this library's reach; that is why the key
 * store is a port of its own with backups of its own (documentation).
 *
 * @param cipher Copied; its `ctx` is borrowed for the key store's life.
 * @param out_keystore Receives the key store; close it with
 *   gdefstor_keystore_destroy().
 * @return ::GDEFSPG_OK, ::GDEFSPG_ERR_INVALID, ::GDEFSPG_ERR_OOM, or the
 *   port's failure.
 */
GDEFSPG_API GDEFSPG_Result gdefspg_keystore_create(
    const GDEFSPG_Options * options, const GDEFSTOR_KeyCipher * cipher,
    const GDEFSPG_Allocator * allocator, GDEFSTOR_KeyStore ** out_keystore);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GDEFSPG_MODEL_H */
