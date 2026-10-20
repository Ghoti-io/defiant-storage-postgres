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
 * The Postgres dialect. It is complete by the SQL layer's own check; and the
 * one clause in it that changes answers, the byte collation on every text
 * column, is seen to matter: on a database whose collation is not bytes (the
 * test server's is the image's en_US.utf8), text sorts and compares by bytes
 * exactly as gdefstor_model_scan() does (the port has no range on text). A database that already
 * compares by bytes could not show the clause missing, so the test refuses
 * to pass on one.
 */

#include <gtest/gtest.h>

#include "model_util.h"
#include "pg_rig.h"

#include <string>
#include <vector>

using namespace tu;

namespace {

TEST(PgDialect, TheDialectPassesTheSqlLayersCheck) {
  GDEFSSQL_Dialect d;
  gdefspg_dialect(&d);
  EXPECT_EQ(GDEFSSQL_OK, gdefssql_dialect_check(&d));
  EXPECT_STREQ("COLLATE \"C\"", d.text_collation);
  EXPECT_EQ(0, d.nulls_low);
  EXPECT_EQ(1, d.placeholder_numbered);
  EXPECT_STREQ("CONCURRENTLY ", d.index_build);
  EXPECT_NE(nullptr, d.index_valid);
  EXPECT_NE(nullptr, d.index_drop);
  EXPECT_NE(nullptr, d.qualifier_create);
  EXPECT_NE(nullptr, d.store_exists);
  EXPECT_STREQ("SMALLINT", d.type_names[GDEFSSQL_COL_BOOLEAN]);
  EXPECT_STREQ("BIGINT", d.type_names[GDEFSSQL_COL_DECIMAL]);
  EXPECT_STREQ("DOUBLE PRECISION", d.type_names[GDEFSSQL_COL_FLOAT]);
}

class PgText : public ::testing::Test {
 protected:
  std::string prefix = pgrig::NewPrefix();
  tee::Tee tee;
  GDEFSTOR_Store * store = nullptr;
  void SetUp() override {
    if (!pgrig::Running()) {
      GTEST_SKIP() << "SKIPPED: postgres";
    }
    GDEFSPG_Options o = pgrig::Options(pgrig::S().conninfo, prefix);
    GDEFSTOR_ModelBackend model;
    ASSERT_EQ(GDEFSPG_OK, gdefspg_model_backend(&o, nullptr, &model));
    tee.on_disagree = [](const std::string & what) {
      ADD_FAILURE() << "Postgres and the reference scan disagree:\n" << what;
    };
    GDEFSTOR_ModelBackend teed = tee::TeeOf(&tee, model);
    GDEFSTOR_Backend kv = pgrig::EmptyKv();
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_store_create_model(&kv, &teed, nullptr,
        nullptr, &store));
  }
  void TearDown() override {
    gdefstor_store_destroy(store);
    if (pgrig::Running()) {
      pgrig::DropPrefix(pgrig::S().conninfo, prefix);
    }
  }
};

/* By bytes, as gdefstor_model_scan() and COLLATE "C" order them. */
TEST_F(PgText, TextSortsAndComparesByBytes) {
  const std::string c = pgrig::S().collation;
  ASSERT_TRUE(c != "C" && c != "POSIX" && c != "ucs_basic")
      << "the database compares text by bytes already (" << c << "): this "
      << "test could not see the collation clause go missing";
  ASSERT_EQ(GDEFSTOR_OK, Declare(store, "site.word",
      {Field("w", GDEFSTOR_FIELD_TEXT, GDEFSTOR_FIELD_INDEXED)}));
  /* Bytes: digits, upper case, '_', lower case, then UTF-8 above 0x7f. A
   * language collation puts these in another order: case folded together,
   * '_' ignored or first, the accented letter beside its base. */
  const std::vector<std::string> words = {"b", "\xc3\x84pfel", "B", "_x", "a",
      "Z", "10", "9", "aB", "ab"};
  for (size_t i = 0; i < words.size(); i++) {
    Node n;
    n.Set("w", Txt(words[i]));
    ASSERT_EQ(GDEFSTOR_OK, Put(store, "site.word", "w" + std::to_string(i), n,
        0, true));
  }
  GDEFSTOR_QuerySort s;
  std::memset(&s, 0, sizeof s);
  s.key.kind = GDEFSTOR_QUERY_FIELD;
  s.key.field = "w";
  s.key.field_len = 1;
  GDEFSTOR_Query q;
  std::memset(&q, 0, sizeof q);
  q.size = sizeof q;
  q.language = "en";
  q.language_len = 2;
  q.sorts = &s;
  q.sort_count = 1;
  q.limit = 100;
  std::vector<std::string> ids;
  uint64_t total = 0;
  ASSERT_EQ(GDEFSTOR_OK, Ask(store, "site.word", q, &ids, &total));
  /* 10 9 B Z _x a aB ab b Äpfel */
  EXPECT_EQ((std::vector<std::string>{"w6", "w7", "w2", "w5", "w3", "w4", "w8",
                "w9", "w0", "w1"}),
      ids);
  /* Equality is by bytes too: "B" is not "b", nor "Äpfel" "apfel". */
  V vb = Txt("B"); /* ValueOf borrows its bytes: kept alive here */
  V va = Txt("apfel");
  GDEFSTOR_Value in[2] = {ValueOf(vb), ValueOf(va)};
  GDEFSTOR_QueryNode eq;
  std::memset(&eq, 0, sizeof eq);
  eq.op = GDEFSTOR_QUERY_IN;
  eq.left.kind = GDEFSTOR_QUERY_FIELD;
  eq.left.field = "w";
  eq.left.field_len = 1;
  eq.right.kind = GDEFSTOR_QUERY_VALUES;
  eq.right.values = in;
  eq.right.value_count = 2;
  q.where = &eq;
  ASSERT_EQ(GDEFSTOR_OK, Ask(store, "site.word", q, &ids, &total));
  EXPECT_EQ((std::vector<std::string>{"w2"}), ids);
  EXPECT_EQ(0, tee.disagreements) << tee.first_disagreement;
}

}  // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return pgrig::RunAll();
}
