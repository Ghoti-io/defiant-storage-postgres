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
 * Connections (AD-14). Two tenants on one pooled connection: nothing of one
 * is left on it for the other, no session search_path, no setting a session
 * made, no transaction, no temporary schema, and the prepared statements are
 * the tenant's own. A connection that saw an error is never handed out
 * again. Every connection carries lock_timeout, so a step that meets a lock
 * fails instead of waiting. A schema of another layout version, or someone
 * else's, is refused and left alone; a read of a tenant with no store holds
 * nothing.
 */

#include <gtest/gtest.h>

#include "model_util.h"
#include "pg_rig.h"
#include "size_structs.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace tu;

namespace {

const int T = GDEFSTOR_FIELD_TEXT;
const int I = GDEFSTOR_FIELD_INTEGER;
const unsigned IDX = GDEFSTOR_FIELD_INDEXED;

struct Rec {
  std::string sql;
  std::string tenant;
  int pid = 0;
  int exec = 0;
  int in_txn = 0;
  int cached = 0;
  int failed = 0;
  size_t stats_size = 0; /* the size of the statistics it came in */
};

/* What a connection held when it went back to the pool. */
struct Idle {
  int pid = 0;
  std::string search_path;
  std::string session_settings;
  std::string lock_timeout;
  std::string temp_schema;
  int status = 0;
};

struct Seen {
  std::vector<Rec> recs;
  std::vector<Idle> idles;
  std::vector<std::string> refusals;
  std::string default_search_path;
};

void OnStatement(void * ctx, const GDEFSPG_StatementStats * s) {
  Seen * seen = static_cast<Seen *>(ctx);
  Rec r;
  r.stats_size = s->size;
  r.sql.assign(s->sql, s->sql_len);
  r.tenant.assign(s->tenant, s->tenant_len);
  r.pid = s->backend_pid;
  r.exec = s->exec;
  r.in_txn = s->in_transaction;
  r.cached = s->cached;
  r.failed = s->failed;
  seen->recs.push_back(r);
}

std::string Ask1(PGconn * c, const char * sql) {
  PGresult * r = PQexec(c, sql);
  std::string out = PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0
                        ? std::string(PQgetvalue(r, 0, 0))
                        : std::string("<error: ") + PQresultErrorMessage(r) + ">";
  PQclear(r);
  return out;
}

/* Read what a pooled connection carries, without changing it. */
void OnIdle(void * ctx, void * pgconn) {
  Seen * seen = static_cast<Seen *>(ctx);
  PGconn * c = static_cast<PGconn *>(pgconn);
  Idle i;
  i.pid = PQbackendPID(c);
  i.status = (int)PQtransactionStatus(c);
  i.search_path = Ask1(c, "SHOW search_path");
  i.session_settings = Ask1(c,
      "SELECT coalesce(string_agg(name, ',' ORDER BY name), '') FROM "
      "pg_settings WHERE source = 'session'");
  i.lock_timeout = Ask1(c, "SHOW lock_timeout");
  i.temp_schema = Ask1(c, "SELECT pg_my_temp_schema()::oid::text");
  seen->idles.push_back(i);
}

void OnRefusal(void * ctx, int result, const char * text, size_t len) {
  (void)result;
  static_cast<Seen *>(ctx)->refusals.emplace_back(text, len);
}

class Pool : public ::testing::Test {
 protected:
  Seen seen;
  std::string prefix = pgrig::NewPrefix();
  GDEFSTOR_Store * store = nullptr;

  void SetUp() override {
    if (!pgrig::Running()) {
      GTEST_SKIP() << "SKIPPED: postgres";
    }
    pgrig::Side side(pgrig::S().conninfo);
    seen.default_search_path = side.One("SHOW search_path");
  }
  void TearDown() override {
    gdefstor_store_destroy(store);
    store = nullptr;
    if (pgrig::Running()) {
      pgrig::DropPrefix(pgrig::S().conninfo, prefix);
    }
  }
  GDEFSPG_Options Opts(size_t idle = 8) {
    GDEFSPG_Options o = pgrig::Options(pgrig::S().conninfo, prefix);
    o.idle_connections = idle;
    o.lock_timeout_ms = 300;
    o.on_statement = OnStatement;
    o.on_statement_ctx = &seen;
    o.on_idle = OnIdle;
    o.on_idle_ctx = &seen;
    o.on_refusal = OnRefusal;
    o.on_refusal_ctx = &seen;
    return o;
  }
  void Open(size_t idle = 8) {
    GDEFSPG_Options o = Opts(idle);
    ASSERT_EQ(GDEFSPG_OK, gdefspg_model_store_create(&o, nullptr, nullptr,
        &store));
  }
  std::string Schema(const char * tenant) {
    char b[64];
    EXPECT_EQ(GDEFSPG_OK, gdefspg_schema_name(prefix.c_str(), tenant,
        std::strlen(tenant), b, sizeof b));
    return b;
  }
  std::vector<std::string> TitlesOf(const char * tenant, const char * title) {
    V vt = Txt(title); /* ValueOf borrows its bytes: kept alive here */
    GDEFSTOR_Value v = ValueOf(vt);
    GDEFSTOR_QueryNode leaf;
    std::memset(&leaf, 0, sizeof leaf);
    leaf.op = GDEFSTOR_QUERY_EQ;
    leaf.left.kind = GDEFSTOR_QUERY_FIELD;
    leaf.left.field = "title";
    leaf.left.field_len = 5;
    leaf.right.kind = GDEFSTOR_QUERY_VALUES;
    leaf.right.values = &v;
    leaf.right.value_count = 1;
    GDEFSTOR_Query q;
    std::memset(&q, 0, sizeof q);
    q.size = sizeof q;
    q.language = "en";
    q.language_len = 2;
    q.where = &leaf;
    q.limit = 50;
    std::vector<std::string> ids;
    uint64_t total = 0;
    EXPECT_EQ(GDEFSTOR_OK, Ask(store, "site.note", q, &ids, &total, tenant));
    return ids;
  }
};

/* AD-34: a struct with a size member is accepted at exactly its own size. The
 * two here are the options a store is opened with and the statistics it tells
 * the observer of each statement (size_structs.h lists every struct). */
TEST_F(Pool, TheStructsHereAreAcceptedAtOnlyTheirOwnSize) {
  Open(1);
  ASSERT_EQ(GDEFSTOR_OK, Declare(store, "site.note",
      {Field("title", T, IDX), Field("n", I)}, "ta"));
  ASSERT_FALSE(seen.recs.empty());
  const std::map<std::string, size_structs::Probe> probes = {
      {"GDEFSPG_Options", [&](size_t size) {
         GDEFSPG_Options o = Opts();
         o.size = size;
         GDEFSTOR_ModelBackend m;
         GDEFSPG_Result rc = gdefspg_model_backend(&o, nullptr, &m);
         if (rc == GDEFSPG_OK) {
           m.destroy(m.ctx); /* nothing connected yet */
         }
         return rc == GDEFSPG_OK;
       }},
      /* The library fills it: whether it filled the size asked about. */
      {"GDEFSPG_StatementStats", [&](size_t size) {
         return seen.recs.back().stats_size == size;
       }},
  };
  size_structs::Run(size_structs::Postgres(), "tests/test_pool.cpp", probes);
}

/* AD-25's gate: two tenants alternate on one pooled connection, and the
 * connection never carries one into the other. */
TEST_F(Pool, TwoTenantsAlternateOnOneConnectionAndCarryNothing) {
  Open(1);
  for (const char * t : {"ta", "tb"}) {
    ASSERT_EQ(GDEFSTOR_OK, Declare(store, "site.note",
        {Field("title", T, IDX), Field("n", I)}, t));
  }
  for (int round = 0; round < 4; round++) {
    for (const char * t : {"ta", "tb"}) {
      Node n;
      n.Set("title", Txt(std::string("same-") + t)).Set("n", Num(I, round));
      ASSERT_EQ(GDEFSTOR_OK, Put(store, "site.note",
          std::string(t) + "-" + std::to_string(round), n, 0, true, nullptr,
          "", 5, t));
      std::string other = std::string("same-") + (t[1] == 'a' ? "tb" : "ta");
      EXPECT_TRUE(TitlesOf(t, other.c_str()).empty()) << t;
    }
  }
  /* Each tenant sees its own rows and only them. */
  EXPECT_EQ(4u, TitlesOf("ta", "same-ta").size());
  EXPECT_EQ(4u, TitlesOf("tb", "same-tb").size());
  EXPECT_TRUE(TitlesOf("ta", "same-tb").empty());
  EXPECT_TRUE(TitlesOf("tb", "same-ta").empty());
  /* A connection the pool handed to both tenants, alternately. */
  std::map<int, std::vector<std::string>> by_pid;
  for (const Rec & r : seen.recs) {
    auto & v = by_pid[r.pid];
    if (v.empty() || v.back() != r.tenant) {
      v.push_back(r.tenant);
    }
  }
  size_t most = 0;
  for (const auto & kv : by_pid) {
    most = std::max(most, kv.second.size());
  }
  EXPECT_GE(most, 8u) << "no pooled connection went from one tenant to the "
                         "other and back: the test saw nothing";
  /* Every time it went back to the pool it carried nothing of a tenant. */
  ASSERT_GT(seen.idles.size(), 10u);
  for (const Idle & i : seen.idles) {
    EXPECT_EQ(seen.default_search_path, i.search_path);
    EXPECT_EQ("", i.session_settings) << "a session-level setting was left";
    EXPECT_EQ("0", i.temp_schema);
    EXPECT_EQ((int)PQTRANS_IDLE, i.status);
    EXPECT_EQ("300ms", i.lock_timeout);
  }
  /* Every statement names its tenant's schema, never the other's, and a
   * prepared statement is only ever reused on the connection, and by the
   * tenant, that prepared it: the catalog queries' texts are the same for
   * both tenants, so a cache keyed by text alone would hand one tenant the
   * other's. */
  std::set<std::string> prepared; /* pid, tenant, text */
  for (const Rec & r : seen.recs) {
    if (r.exec) {
      continue;
    }
    std::string other = Schema(r.tenant == "ta" ? "tb" : "ta");
    EXPECT_EQ(std::string::npos, r.sql.find("\"" + other + "\"")) << r.sql;
    std::string key = std::to_string(r.pid) + "|" + r.tenant + "|" + r.sql;
    if (r.cached) {
      EXPECT_EQ(1u, prepared.count(key))
          << "a statement reused that this tenant did not prepare here: " << r.sql;
    } else {
      prepared.insert(key);
    }
  }
  int cached = 0;
  for (const Rec & r : seen.recs) {
    cached += r.cached;
  }
  EXPECT_GT(cached, 20) << "the statement cache was never used";
}

/* A failed statement discards its connection: no statement after it ever
 * runs on that server process, and the next call gets a fresh one. */
TEST_F(Pool, AFailedStatementDiscardsItsConnection) {
  Open(4);
  ASSERT_EQ(GDEFSTOR_OK, Declare(store, "site.note", {Field("title", T)}));
  Node n;
  n.Set("title", Txt("one"));
  ASSERT_EQ(GDEFSTOR_OK, Put(store, "site.note", "n1", n, 0, true));
  /* Hold the counter row's table so the next create's UPDATE waits past
   * lock_timeout (300ms) and fails. */
  pgrig::Side side(pgrig::S().conninfo);
  ASSERT_TRUE(side.Ok());
  ASSERT_TRUE(side.Exec("BEGIN"));
  ASSERT_TRUE(side.Exec("LOCK TABLE \"" + Schema("t1") +
                        "\".\"dss_counter\" IN ACCESS EXCLUSIVE MODE"));
  size_t mark = seen.recs.size();
  size_t idle_mark = seen.idles.size();
  n.Set("title", Txt("two"));
  GDEFSTOR_Result rc = Put(store, "site.note", "n2", n, 0, true);
  EXPECT_EQ(GDEFSTOR_ERR_BUSY, rc) << gdefstor_result_string(rc);
  ASSERT_TRUE(side.Exec("ROLLBACK"));
  int bad = 0;
  size_t after = seen.recs.size();
  for (size_t i = mark; i < after; i++) {
    if (seen.recs[i].failed) {
      bad = seen.recs[i].pid;
    }
  }
  ASSERT_NE(0, bad) << "no statement failed";
  ASSERT_EQ(GDEFSTOR_OK, Put(store, "site.note", "n2", n, 0, true));
  EXPECT_EQ((std::vector<std::string>{"n2"}), TitlesOf("t1", "two"));
  bool fresh = false;
  for (size_t i = after; i < seen.recs.size(); i++) {
    EXPECT_NE(bad, seen.recs[i].pid)
        << "a connection that saw an error was used again: "
        << seen.recs[i].sql;
    fresh = true;
  }
  EXPECT_TRUE(fresh);
  for (size_t i = idle_mark; i < seen.idles.size(); i++) {
    EXPECT_NE(bad, seen.idles[i].pid)
        << "a connection that saw an error went back to the pool";
  }
}

/* Postgres steps set lock_timeout (AD-15): a declaration whose step meets a
 * lock fails after the timeout, writes nothing, and the next one goes
 * through. */
TEST_F(Pool, ASchemaStepThatMeetsALockFailsInsteadOfWaiting) {
  Open();
  ASSERT_EQ(GDEFSTOR_OK, Declare(store, "site.note", {Field("title", T)}));
  pgrig::Side side(pgrig::S().conninfo);
  ASSERT_TRUE(side.Exec("BEGIN"));
  ASSERT_TRUE(side.Exec("LOCK TABLE \"" + Schema("t1") +
                        "\".\"t1_g0\" IN ACCESS EXCLUSIVE MODE"));
  GDEFSTOR_Result rc = Declare(store, "site.note",
      {Field("title", T), Field("body", T)});
  EXPECT_EQ(GDEFSTOR_ERR_BUSY, rc) << gdefstor_result_string(rc);
  ASSERT_TRUE(side.Exec("ROLLBACK"));
  GDEFSTOR_Type * type = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_type_get(store, "t1", 2, "site.note", 9,
      &type));
  size_t at = 0;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_type_find(type, "body", 4, &at));
  gdefstor_type_free(type);
  EXPECT_EQ(GDEFSTOR_OK, Declare(store, "site.note",
      {Field("title", T), Field("body", T)}));
  bool stepped = false;
  for (const Rec & r : seen.recs) {
    stepped |= r.exec && r.sql.rfind("ALTER TABLE", 0) == 0;
  }
  EXPECT_TRUE(stepped);
}

/* A schema whose dss_schema says a newer version is refused, by reads and
 * writes alike, and left as it is; someone else's schema is refused too. */
TEST_F(Pool, ASchemaOfAnotherLayoutIsRefusedAndLeftAlone) {
  Open();
  ASSERT_EQ(GDEFSTOR_OK, Declare(store, "site.note", {Field("title", T)}));
  gdefstor_store_destroy(store);
  store = nullptr;
  pgrig::Side side(pgrig::S().conninfo);
  ASSERT_TRUE(side.Exec("UPDATE \"" + Schema("t1") +
                        "\".\"dss_schema\" SET version = version + 1"));
  std::string tables = side.One("SELECT count(*) FROM pg_tables WHERE "
                                "schemaname = '" + Schema("t1") + "'");
  Open();
  GDEFSTOR_Type * type = nullptr;
  EXPECT_EQ(GDEFSTOR_ERR_UNSUPPORTED, gdefstor_type_get(store, "t1", 2,
      "site.note", 9, &type));
  EXPECT_EQ(GDEFSTOR_ERR_UNSUPPORTED, Declare(store, "site.other",
      {Field("x", T)}));
  ASSERT_FALSE(seen.refusals.empty());
  EXPECT_NE(std::string::npos, seen.refusals[0].find("version 2"))
      << seen.refusals[0];
  EXPECT_EQ(tables, side.One("SELECT count(*) FROM pg_tables WHERE "
                             "schemaname = '" + Schema("t1") + "'"));
  /* A schema of someone else's tables. */
  ASSERT_TRUE(side.Exec("CREATE SCHEMA \"" + Schema("t2") + "\""));
  ASSERT_TRUE(side.Exec("CREATE TABLE \"" + Schema("t2") + "\".notes (x text)"));
  EXPECT_EQ(GDEFSTOR_ERR_FORMAT, Declare(store, "site.note", {Field("x", T)},
      "t2"));
  EXPECT_EQ("1", side.One("SELECT count(*) FROM pg_tables WHERE "
                          "schemaname = '" + Schema("t2") + "'"));
}

/* A read of a tenant with no store holds nothing and makes nothing. */
TEST_F(Pool, AReadOfATenantWithNoStoreMakesNothing) {
  Open();
  GDEFSTOR_Type * type = nullptr;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_type_get(store, "t9", 2,
      "site.note", 9, &type));
  uint64_t n = 7;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_entity_count(store, "t9", 2,
      "site.note", 9, "en", 2, nullptr, &n));
  pgrig::Side side(pgrig::S().conninfo);
  EXPECT_EQ("0", side.One("SELECT count(*) FROM pg_namespace WHERE nspname = '" +
                          Schema("t9") + "'"));
}

/* Tenant names: the schema is the prefix and the hex of the tenant, and a
 * tenant too long to name within 63 bytes is refused, never truncated. */
TEST(PgNames, ASchemaIsThePrefixAndTheTenantInHex) {
  char b[64];
  ASSERT_EQ(GDEFSPG_OK, gdefspg_schema_name("ds_", "t1", 2, b, sizeof b));
  EXPECT_STREQ("ds_7431", b);
  ASSERT_EQ(GDEFSPG_OK, gdefspg_schema_name(nullptr, "\xff", 1, b, sizeof b));
  EXPECT_STREQ("ds_ff", b);
  std::string longest(30, 'x');
  EXPECT_EQ(GDEFSPG_OK, gdefspg_schema_name("ds_", longest.data(),
      longest.size(), b, sizeof b));
  EXPECT_EQ(63u, std::strlen(b));
  longest += "x";
  EXPECT_EQ(GDEFSPG_ERR_LIMIT, gdefspg_schema_name("ds_", longest.data(),
      longest.size(), b, sizeof b));
  for (const char * bad : {"", "Ds_", "9a", "pg_x", "a-b", "a.b",
           "abcdefghijklmnop"}) {
    EXPECT_EQ(GDEFSPG_ERR_INVALID, gdefspg_schema_name(bad, "t", 1, b,
        sizeof b)) << bad;
  }
  EXPECT_EQ(GDEFSPG_ERR_INVALID, gdefspg_schema_name("ds_", "t", 1, b, 63));
}

TEST(PgNames, OptionsAreChecked) {
  GDEFSPG_Options o;
  gdefspg_options_default(&o);
  GDEFSTOR_ModelBackend m;
  EXPECT_EQ(GDEFSPG_ERR_INVALID, gdefspg_model_backend(&o, nullptr, &m))
      << "no conninfo";
  o.conninfo = "dbname=x";
  o.lock_timeout_ms = 0;
  EXPECT_EQ(GDEFSPG_ERR_INVALID, gdefspg_model_backend(&o, nullptr, &m))
      << "a step may never wait forever";
  o.lock_timeout_ms = 10;
  o.schema_prefix = "pg_";
  EXPECT_EQ(GDEFSPG_ERR_INVALID, gdefspg_model_backend(&o, nullptr, &m));
  o.schema_prefix = nullptr;
  o.size = 3;
  EXPECT_EQ(GDEFSPG_ERR_INVALID, gdefspg_model_backend(&o, nullptr, &m));
  o.size = sizeof o;
  ASSERT_EQ(GDEFSPG_OK, gdefspg_model_backend(&o, nullptr, &m));
  m.destroy(m.ctx); /* nothing connected yet */
}

}  // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return pgrig::RunAll();
}
