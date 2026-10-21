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
 * The throwaway server, as the tests see it. tools/pg-test.sh starts it
 * (podman or docker, the pinned postgres:18 digest, a random port) and hands
 * the tests its connection string in GDEFSPG_TEST_CONNINFO; this header
 * decides, once per test program, what the Postgres arm does:
 *
 * - GDEFSPG_TEST_CONNINFO set and the server answers: the tests run.
 * - GDEFSPG_TEST_SKIP_SERVER=1 and no connection string: none of the
 *   program's tests run but PgArmGate, which is skipped, and the program
 *   prints `SKIPPED: postgres`. gtest reports one test skipped and none
 *   passed.
 * - Neither, or a server that does not answer: PgArmGate fails, naming the
 *   fix, and nothing else runs.
 *
 * Each store a test opens gets schema names of its own (a prefix unique to
 * the process and the store), and is dropped when it is closed, so tests
 * never see each other's rows and the server does not fill up.
 */

#ifndef GDEFSPG_TESTS_PG_RIG_H
#define GDEFSPG_TESTS_PG_RIG_H

#include <gtest/gtest.h>

#include <ghoti.io/defiant-storage-postgres/defiant-storage-postgres.h>
#include <libpq-fe.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

namespace pgrig {

/* How a test program meets the server. */
enum Arm { ARM_UNKNOWN = 0, ARM_RUN, ARM_SKIP, ARM_MISSING, ARM_DOWN };

inline const char * kFix =
    "the Postgres tests need a server. Run them through tools/pg-test.sh, "
    "which starts docker.io/library/postgres@sha256:885953109528ad3dfc90362b1"
    "a6f50a78620b5315be19f187753d267e484dc5b with podman (or docker) on a "
    "random port and runs `make test` in the image the Containerfile builds; "
    "on the EVO-X2, through tools/evo run --host-after (documentation/"
    "postgres.md). To skip the Postgres arm instead, set "
    "GDEFSPG_TEST_SKIP_SERVER=1: it is then reported as SKIPPED: postgres, "
    "never as passed.";

struct State {
  Arm arm = ARM_UNKNOWN;
  std::string conninfo;
  std::string keys_conninfo;
  std::string why;
  std::string version;
  std::string collation;
};

inline State & S() {
  static State s;
  return s;
}

/* A connection the test itself holds, apart from any store's. */
struct Side {
  PGconn * c = nullptr;
  explicit Side(const std::string & conninfo) {
    c = PQconnectdb(conninfo.c_str());
    if (Ok()) {
      /* The test's own connection: no NOTICE for every cascaded drop. */
      PQclear(PQexec(c, "SET client_min_messages TO warning"));
    }
  }
  ~Side() {
    if (c != nullptr) {
      PQfinish(c);
    }
  }
  Side(const Side &) = delete;
  Side & operator=(const Side &) = delete;
  bool Ok() const {
    return c != nullptr && PQstatus(c) == CONNECTION_OK;
  }
  /* Rows of text; an error fails the test that ran it. */
  std::vector<std::vector<std::string>> Rows(const std::string & sql,
      const std::vector<std::string> & params = {}) {
    std::vector<std::vector<std::string>> out;
    std::vector<const char *> v;
    for (const auto & p : params) {
      v.push_back(p.c_str());
    }
    PGresult * r = PQexecParams(c, sql.c_str(), (int)v.size(), nullptr,
        v.empty() ? nullptr : v.data(), nullptr, nullptr, 0);
    ExecStatusType st = PQresultStatus(r);
    if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK) {
      ADD_FAILURE() << "side statement failed: " << PQresultErrorMessage(r)
                    << " in: " << sql;
    } else {
      for (int i = 0; i < PQntuples(r); i++) {
        std::vector<std::string> row;
        for (int k = 0; k < PQnfields(r); k++) {
          row.push_back(PQgetisnull(r, i, k) ? std::string("<null>")
                                             : std::string(PQgetvalue(r, i, k)));
        }
        out.push_back(row);
      }
    }
    PQclear(r);
    return out;
  }
  std::string One(const std::string & sql,
      const std::vector<std::string> & params = {}) {
    auto rows = Rows(sql, params);
    return rows.empty() || rows[0].empty() ? std::string("<none>") : rows[0][0];
  }
  bool Exec(const std::string & sql) {
    PGresult * r = PQexec(c, sql.c_str());
    ExecStatusType st = PQresultStatus(r);
    bool ok = st == PGRES_COMMAND_OK || st == PGRES_TUPLES_OK;
    if (!ok) {
      ADD_FAILURE() << "side statement failed: " << PQresultErrorMessage(r)
                    << " in: " << sql;
    }
    PQclear(r);
    return ok;
  }
};

/* With dbname replaced: the key store's database. */
inline std::string WithDb(const std::string & conninfo, const char * db) {
  return conninfo + " dbname=" + db;
}

/* Decide the arm. Called once, by the environment. */
inline void Decide() {
  State & s = S();
  const char * ci = std::getenv("GDEFSPG_TEST_CONNINFO");
  const char * skip = std::getenv("GDEFSPG_TEST_SKIP_SERVER");
  if (ci == nullptr || ci[0] == '\0') {
    if (skip != nullptr && std::strcmp(skip, "1") == 0) {
      s.arm = ARM_SKIP;
      s.why = "GDEFSPG_TEST_SKIP_SERVER=1";
      return;
    }
    s.arm = ARM_MISSING;
    s.why = std::string("GDEFSPG_TEST_CONNINFO is not set: ") + kFix;
    return;
  }
  s.conninfo = ci;
  Side side(s.conninfo);
  if (!side.Ok()) {
    s.arm = ARM_DOWN;
    s.why = std::string("the server in GDEFSPG_TEST_CONNINFO does not answer (") +
            (side.c != nullptr ? PQerrorMessage(side.c) : "no memory") +
            "): " + kFix;
    return;
  }
  s.version = side.One("SELECT version()");
  s.collation = side.One(
      "SELECT datcollate FROM pg_database WHERE datname = current_database()");
  /* The key store's database of its own (AD-13), made once per server. */
  if (side.One("SELECT count(*) FROM pg_database WHERE datname = 'gdefspg_keys'") ==
      "0") {
    PGresult * r = PQexec(side.c, "CREATE DATABASE gdefspg_keys");
    /* Another test program may have made it first. */
    PQclear(r);
  }
  s.keys_conninfo = WithDb(s.conninfo, "gdefspg_keys");
  s.arm = ARM_RUN;
}

/* What a Postgres test program's main() calls in place of RUN_ALL_TESTS().
 *
 * With the server, every test runs (PgArmGate among them, which then
 * passes). Without one, only PgArmGate runs: skipped under
 * GDEFSPG_TEST_SKIP_SERVER=1, failed and naming the fix otherwise. The arm is
 * never skipped from a global environment's SetUp: gtest counts the tests
 * such a skip stops as passed ("[  PASSED  ] N tests"), which is the very
 * report AD-25 forbids, and PgHarness.ASkippedArmIsNeverReportedAsPassing
 * saw it do so. */
inline int RunAll() {
  Decide();
  State & s = S();
  switch (s.arm) {
  case ARM_RUN:
    std::printf("postgres: %s (collation %s)\n", s.version.c_str(),
        s.collation.c_str());
    break;
  case ARM_SKIP:
    std::printf("SKIPPED: postgres (%s): the Postgres arm did not run and "
                "is not counted as passed\n", s.why.c_str());
    GTEST_FLAG_SET(filter, "PgArmGate.*");
    break;
  default:
    GTEST_FLAG_SET(filter, "PgArmGate.*");
    break;
  }
  std::fflush(stdout);
  return RUN_ALL_TESTS();
}

inline bool Running() {
  return S().arm == ARM_RUN;
}

}  // namespace pgrig

/* The arm itself, as a test: it passes with the server, is skipped without
 * one when the caller chose that, and fails, naming the fix, otherwise. */
TEST(PgArmGate, TheServerIsThere) {
  switch (pgrig::S().arm) {
  case pgrig::ARM_RUN:
    return;
  case pgrig::ARM_SKIP:
    GTEST_SKIP() << "SKIPPED: postgres";
  default:
    FAIL() << pgrig::S().why;
  }
}

namespace pgrig {


/* A schema prefix of this process and this store: never another's. */
inline std::string NewPrefix() {
  static unsigned n = 0;
  char b[GDEFSPG_MAX_SCHEMA_PREFIX + 1];
  std::snprintf(b, sizeof b, "p%x_%u_", (unsigned)getpid() & 0xfffffu, ++n);
  return b;
}

/* Drop every schema of @p prefix in the database of @p conninfo. */
inline void DropPrefix(const std::string & conninfo, const std::string & prefix) {
  Side side(conninfo);
  if (!side.Ok()) {
    return;
  }
  auto rows = side.Rows("SELECT nspname FROM pg_namespace WHERE "
                        "starts_with(nspname, $1)", {prefix});
  for (const auto & r : rows) {
    side.Exec("DROP SCHEMA \"" + r[0] + "\" CASCADE");
  }
}

inline GDEFSPG_Options Options(const std::string & conninfo,
    const std::string & prefix) {
  GDEFSPG_Options o;
  gdefspg_options_default(&o);
  o.conninfo = conninfo.c_str();
  o.schema_prefix = prefix.c_str();
  return o;
}

inline GDEFSTOR_Result ToPort(GDEFSPG_Result rc) {
  switch (rc) {
  case GDEFSPG_OK:
    return GDEFSTOR_OK;
  case GDEFSPG_ERR_IO:
    return GDEFSTOR_ERR_IO;
  case GDEFSPG_ERR_OOM:
    return GDEFSTOR_ERR_OOM;
  case GDEFSPG_ERR_INVALID:
    return GDEFSTOR_ERR_INVALID;
  case GDEFSPG_ERR_BUSY:
    return GDEFSTOR_ERR_BUSY;
  case GDEFSPG_ERR_LIMIT:
    return GDEFSTOR_ERR_LIMIT;
  case GDEFSPG_ERR_UNSUPPORTED:
    return GDEFSTOR_ERR_UNSUPPORTED;
  case GDEFSPG_ERR_FORMAT:
    return GDEFSTOR_ERR_FORMAT;
  case GDEFSPG_ERR_CORRUPT:
    return GDEFSTOR_ERR_CORRUPT;
  default:
    return GDEFSTOR_ERR_INTERNAL;
  }
}

}  // namespace pgrig

#endif /* GDEFSPG_TESTS_PG_RIG_H */
