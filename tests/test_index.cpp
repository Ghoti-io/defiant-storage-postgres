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
 * Index builds on Postgres (AD-15). A declaration's step holds no CREATE
 * INDEX: each index is built after the step commits, with CREATE INDEX
 * CONCURRENTLY, outside any transaction. A field reads "indexing" until its
 * build is done. A concurrent build that cannot finish (here: another
 * transaction holds an older snapshot, so the build waits past lock_timeout)
 * leaves an invalid index; the next commit drops it and builds it again.
 * Queries answer the same throughout.
 */

#include <gtest/gtest.h>

#include "model_util.h"
#include "pg_rig.h"

#include <string>
#include <vector>

using namespace tu;

namespace {

const int T = GDEFSTOR_FIELD_TEXT;
const int I = GDEFSTOR_FIELD_INTEGER;
const unsigned IDX = GDEFSTOR_FIELD_INDEXED;

struct Rec {
  std::string sql;
  int exec = 0;
  int in_txn = 0;
  int failed = 0;
};

void OnStatement(void * ctx, const GDEFSPG_StatementStats * s) {
  Rec r;
  r.sql.assign(s->sql, s->sql_len);
  r.exec = s->exec;
  r.in_txn = s->in_transaction;
  r.failed = s->failed;
  static_cast<std::vector<Rec> *>(ctx)->push_back(r);
}

bool IsIndexBuild(const std::string & s) {
  return s.rfind("CREATE INDEX ", 0) == 0 || s.rfind("CREATE UNIQUE INDEX ", 0) == 0;
}

class PgIndex : public ::testing::Test {
 protected:
  std::vector<Rec> recs;
  std::string prefix = pgrig::NewPrefix();
  GDEFSTOR_Store * store = nullptr;
  std::string schema;

  void SetUp() override {
    if (!pgrig::Running()) {
      GTEST_SKIP() << "SKIPPED: postgres";
    }
    /* A statement_timeout behind lock_timeout, so that a build that waited
     * on something lock_timeout does not cover still ends. */
    conninfo = pgrig::S().conninfo + " options='-c statement_timeout=30000'";
    GDEFSPG_Options o = pgrig::Options(conninfo, prefix);
    o.lock_timeout_ms = 300;
    o.on_statement = OnStatement;
    o.on_statement_ctx = &recs;
    ASSERT_EQ(GDEFSPG_OK, gdefspg_model_store_create(&o, nullptr, nullptr,
        &store));
    char b[64];
    ASSERT_EQ(GDEFSPG_OK, gdefspg_schema_name(prefix.c_str(), kTenant, 2, b,
        sizeof b));
    schema = b;
  }
  void TearDown() override {
    gdefstor_store_destroy(store);
    if (pgrig::Running()) {
      pgrig::DropPrefix(pgrig::S().conninfo, prefix);
    }
  }
  std::string conninfo;

  int StateOf(const char * path) {
    int st = -1;
    EXPECT_EQ(GDEFSTOR_OK, gdefstor_type_index_state(store, kTenant, 2,
        "shop.box", 8, path, std::strlen(path), &st));
    return st;
  }
  /* What pg_index says of the index: "t", "f" or "<none>". */
  std::string Valid(pgrig::Side & side, const std::string & index) {
    return side.One("SELECT i.indisvalid FROM pg_index i JOIN pg_class c ON "
                    "c.oid = i.indexrelid JOIN pg_namespace n ON n.oid = "
                    "c.relnamespace WHERE n.nspname = $1 AND c.relname = $2",
        {schema, index});
  }
  std::string FieldIndex(pgrig::Side & side) {
    return side.One("SELECT name FROM \"" + schema +
                    "\".dss_name WHERE role = 9");
  }
  std::vector<std::string> Bucket(const char * b) {
    V vb = Txt(b); /* ValueOf borrows its bytes: kept alive here */
    GDEFSTOR_Value v = ValueOf(vb);
    GDEFSTOR_QueryNode leaf;
    std::memset(&leaf, 0, sizeof leaf);
    leaf.op = GDEFSTOR_QUERY_EQ;
    leaf.left.kind = GDEFSTOR_QUERY_FIELD;
    leaf.left.field = "bucket";
    leaf.left.field_len = 6;
    leaf.right.kind = GDEFSTOR_QUERY_VALUES;
    leaf.right.values = &v;
    leaf.right.value_count = 1;
    GDEFSTOR_Query q;
    std::memset(&q, 0, sizeof q);
    q.size = sizeof q;
    q.where = &leaf;
    q.limit = 100;
    std::vector<std::string> ids;
    uint64_t total = 0;
    EXPECT_EQ(GDEFSTOR_OK, Ask(store, "shop.box", q, &ids, &total));
    return ids;
  }
  void Fill(int n) {
    for (int i = 0; i < n; i++) {
      Node t;
      t.Set("bucket", Txt(i % 2 ? "odd" : "even")).Set("n", Num(I, i));
      ASSERT_EQ(GDEFSTOR_OK, Put(store, "shop.box", "b" + std::to_string(i), t,
          0, true));
    }
  }
};

/* Every index of a new type and of a newly indexed field is built with
 * CREATE INDEX CONCURRENTLY, outside every transaction, and is valid. */
TEST_F(PgIndex, EveryIndexIsBuiltConcurrentlyOutsideTheStep) {
  ASSERT_EQ(GDEFSTOR_OK, Declare(store, "shop.box",
      {Field("bucket", T), Field("n", I)}));
  Fill(4);
  EXPECT_EQ(GDEFSTOR_INDEX_NONE, StateOf("bucket"));
  ASSERT_EQ(GDEFSTOR_OK, Declare(store, "shop.box",
      {Field("bucket", T, IDX), Field("n", I, IDX)}));
  EXPECT_EQ(GDEFSTOR_INDEX_READY, StateOf("bucket"));
  EXPECT_EQ(GDEFSTOR_INDEX_READY, StateOf("n"));
  int builds = 0;
  for (const Rec & r : recs) {
    if (IsIndexBuild(r.sql)) {
      builds++;
      EXPECT_FALSE(r.in_txn) << r.sql;
      EXPECT_NE(std::string::npos, r.sql.find(" CONCURRENTLY ")) << r.sql;
      EXPECT_FALSE(r.failed) << r.sql;
    }
  }
  /* dss_type_tno, dss_entity_rid, t1p, then bucket and n. */
  EXPECT_EQ(5, builds);
  pgrig::Side side(pgrig::S().conninfo);
  EXPECT_EQ("5", side.One("SELECT count(*) FROM pg_index i JOIN pg_class c ON "
      "c.oid = i.indexrelid JOIN pg_namespace n ON n.oid = c.relnamespace "
      "WHERE n.nspname = $1 AND i.indisvalid AND NOT i.indisprimary",
      {schema}));
  EXPECT_EQ((std::vector<std::string>{"b0", "b2"}), Bucket("even"));
}

/* A build that cannot finish leaves an invalid index; the field reads
 * "indexing", its queries answer the same, and the next commit drops the
 * invalid index and builds it again. */
TEST_F(PgIndex, AFailedConcurrentBuildIsDroppedAndBuiltAgain) {
  ASSERT_EQ(GDEFSTOR_OK, Declare(store, "shop.box",
      {Field("bucket", T), Field("n", I)}));
  Fill(6);
  pgrig::Side holder(pgrig::S().conninfo);
  pgrig::Side side(pgrig::S().conninfo);
  ASSERT_TRUE(holder.Ok());
  /* An older snapshot that a concurrent build must wait out. */
  ASSERT_TRUE(holder.Exec("BEGIN ISOLATION LEVEL REPEATABLE READ"));
  ASSERT_EQ("1", holder.One("SELECT 1"));
  recs.clear();
  ASSERT_EQ(GDEFSTOR_OK, Declare(store, "shop.box",
      {Field("bucket", T, IDX), Field("n", I)}))
      << "the declaration committed; only its build failed";
  bool failed_build = false;
  for (const Rec & r : recs) {
    failed_build |= IsIndexBuild(r.sql) && r.failed;
  }
  EXPECT_TRUE(failed_build) << "the build did not fail: the test proves nothing";
  std::string index = FieldIndex(side);
  EXPECT_EQ("f", Valid(side, index)) << index;
  EXPECT_EQ(GDEFSTOR_INDEX_INDEXING, StateOf("bucket"));
  EXPECT_EQ((std::vector<std::string>{"b1", "b3", "b5"}), Bucket("odd"));
  ASSERT_TRUE(holder.Exec("COMMIT"));
  /* The next commit of the tenant, whatever it writes. */
  recs.clear();
  Node t;
  t.Set("bucket", Txt("odd")).Set("n", Num(I, 7));
  ASSERT_EQ(GDEFSTOR_OK, Put(store, "shop.box", "b7", t, 0, true));
  bool dropped = false;
  for (const Rec & r : recs) {
    dropped |= r.exec && r.sql.rfind("DROP INDEX CONCURRENTLY IF EXISTS ", 0) == 0;
  }
  EXPECT_TRUE(dropped);
  EXPECT_EQ("t", Valid(side, index));
  EXPECT_EQ(GDEFSTOR_INDEX_READY, StateOf("bucket"));
  EXPECT_EQ((std::vector<std::string>{"b1", "b3", "b5", "b7"}), Bucket("odd"));
}

}  // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return pgrig::RunAll();
}
