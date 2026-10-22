#!/bin/sh
#
# SPDX-License-Identifier: LGPL-3.0-only
#
# Show that each defect planted in the Postgres backend turns a named test
# red: the defects story S4 names (spine AD-25). Each plant edits a scratch
# copy of the real source (defiant-storage's plant-lib.sh), so nothing in src/
# selects a defect. The plants that need the server run only when there is one
# (GDEFSPG_TEST_CONNINFO); without it this gate says so and, unless
# GDEFSPG_TEST_SKIP_SERVER=1 was chosen, fails: it never reports a plant it
# did not run as caught.
#
# The defect "CREATE INDEX inside the step transaction" lives in the SQL
# layer, and defiant-storage-sql's check-sql plants it there; the plant here
# is this executor's form of it.
#
# Usage: check-pg.sh <make> <build-dir> [make args]

set -u

MAKE="${1:?usage: check-pg.sh <make> <build-dir> [make args]}"
BUILD_DIR="${2:?usage: check-pg.sh <make> <build-dir> [make args]}"
shift 2

# The helpers are defiant-storage's, installed with its conformance suites;
# the Makefile names the file.
PLANT_LIB="${GHOTI_PLANT_LIB:-}"
if [ ! -r "$PLANT_LIB" ]; then
  printf '%s: defiant-storage installs the plant helpers (plant-lib.sh) with its conformance suites and none was found at "%s": install defiant-storage first, then run this through make (GHOTI_PLANT_LIB names the file)\n' \
    "$(basename "$0")" "$PLANT_LIB" >&2
  exit 1
fi
. "$PLANT_LIB"
plant_setup "$MAKE" "$BUILD_DIR" "$@"

# A skipped Postgres arm reported as passing: the arm's gate returns instead
# of skipping, so gtest counts it passed.
plant_control testHarness 'PgHarness.*'
plant skipped-arm-passes tests/pg_rig.h testHarness \
  'PgHarness.ASkippedArmIsNeverReportedAsPassing' \
  '  case pgrig::ARM_SKIP:
    GTEST_SKIP() << "SKIPPED: postgres";' \
  '  case pgrig::ARM_SKIP:
    return;'

# The stale-object plant. A plant's copy starts from the finished build, so the
# edit has to be compiled into it; this one is made not to be (the objects that
# depend on the edit are left looking up to date), and the gate has to refuse it
# as a STALE OBJECT. If it ever passes instead, a warm copy could pass with the
# defect never built in. It needs no server, so it runs whenever the gate does.
plant_stale stale-object tests/pg_rig.h testHarness \
  'PgHarness.ASkippedArmIsNeverReportedAsPassing' \
  '  case pgrig::ARM_SKIP:
    GTEST_SKIP() << "SKIPPED: postgres";' \
  '  case pgrig::ARM_SKIP:
    return;'

if [ -z "${GDEFSPG_TEST_CONNINFO:-}" ]; then
  if [ "${GDEFSPG_TEST_SKIP_SERVER:-}" = 1 ]; then
    plant_wait
    [ "$PLANT_FAIL" -eq 0 ] || exit 1
    printf 'check-pg: SKIPPED: postgres: only the harness plant ran; the plants that need a server were not run, and are not reported as caught\n'
    exit 0
  fi
  printf 'check-pg: no Postgres server (GDEFSPG_TEST_CONNINFO is not set): run make test through tools/pg-test.sh, or set GDEFSPG_TEST_SKIP_SERVER=1 to skip the Postgres arm\n' >&2
  exit 1
fi

plant_control testPool 'Pool.*'
plant_control testDialect 'PgText.*:PgDialect.*'
plant_control testIndex 'PgIndex.*'

# A session-level search_path: the tenant selected by connection state, left
# on the pooled connection for the next tenant (AD-14, AD-25).
plant session-search-path src/model.c testPool \
  'Pool.TwoTenantsAlternateOnOneConnectionAndCarryNothing' \
  '  memcpy(c->schema, schema, strlen(schema) + 1);' \
  '  memcpy(c->schema, schema, strlen(schema) + 1);
  {
    char set[96];
    PGresult * sp;
    snprintf(set, sizeof set, "SET search_path TO \"%s\"", schema);
    sp = PQexec(c->pg, set);
    PQclear(sp);
  }'

# A dialect clause dropped: text columns without COLLATE "C" compare and sort
# in the database's language collation, not by bytes.
plant collate-dropped src/model.c testDialect \
  'PgText.TextSortsAndComparesByBytes' \
  '  d->text_collation = "COLLATE \"C\"";' \
  '  d->text_collation = NULL;'

# A connection reused after an error: it goes back to the pool whatever the
# SQL layer said.
plant reused-after-error src/model.c testPool \
  'Pool.AFailedStatementDiscardsItsConnection' \
  '  if (discard || c->bad || PQstatus(c->pg) != CONNECTION_OK ||' \
  '  (void)discard;
  if (PQstatus(c->pg) != CONNECTION_OK ||'

# CREATE INDEX inside a transaction: this executor wraps each index build in
# one, which the server refuses for CONCURRENTLY, so no index is built.
plant index-build-in-transaction src/model.c testIndex \
  'PgIndex.EveryIndexIsBuiltConcurrentlyOutsideTheStep' \
  '  in_txn = in_transaction(c->pg);
  res = PQexec(c->pg, text);' \
  '  if (strncmp(text, "CREATE INDEX", 12) == 0 ||
      strncmp(text, "CREATE UNIQUE INDEX", 19) == 0) {
    PQclear(PQexec(c->pg, "BEGIN"));
  }
  in_txn = in_transaction(c->pg);
  res = PQexec(c->pg, text);
  if (in_txn && (strncmp(text, "CREATE INDEX", 12) == 0 ||
                    strncmp(text, "CREATE UNIQUE INDEX", 19) == 0)) {
    PQclear(PQexec(c->pg, "ROLLBACK"));
  }'

# An erasure that leaves the destroyed key in the table's pages: no vacuum
# after the commit (story S5b).
plant_control testKeys 'PgKeys.*'
plant key-vacuum-dropped src/model.c testKeys \
  'PgKeys.AnErasureVacuumsTheKeyTablesOutsideTheTransaction' \
  '  d->key_vacuum = "VACUUM (FULL) ";' \
  '  d->key_vacuum = NULL;'

plant_finish
