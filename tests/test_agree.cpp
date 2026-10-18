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
 * The agreement sweep on Postgres: seeded random data written alike to a
 * Postgres store, a memory store and a SQLite store, and seeded random
 * queries asked of all three. Every Postgres answer is also answered by
 * gdefstor_model_scan() over the same Postgres rows (the tee), and must equal
 * the memory backend's and the SQLite backend's, page order and total
 * included. The sweep requires that it met the cases that tell engines apart.
 */

#include <gtest/gtest.h>

#include <ghoti.io/defiant-storage-memory/defiant-storage-memory.h>
#include <ghoti.io/defiant-storage-sqlite/defiant-storage-sqlite.h>

#include "pg_rig.h"
#include "query_gen.h"
#include "tmpdir.h"

#include <filesystem>

using namespace tu;

namespace {

struct Seen {
  int queries = 0;
  int nonempty = 0;
  int empty = 0;
  int paged = 0;
  int clamped = 0;
  int partial = 0;
  int sorted = 0;
  int descending = 0;
  int exists = 0;
  int nots = 0;
  int field_field = 0;
  int outer = 0;
};

struct Pg {
  std::string prefix = pgrig::NewPrefix();
  tee::Tee tee;
  GDEFSTOR_Store * store = nullptr;
  ~Pg() {
    gdefstor_store_destroy(store);
    pgrig::DropPrefix(pgrig::S().conninfo, prefix);
  }
};

struct Lite {
  std::string dir = MakeTempDir("gdefspg-agree-");
  GDEFSTOR_Store * store = nullptr;
  ~Lite() {
    gdefstor_store_destroy(store);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
};

void OpenPg(Pg & p, uint32_t seed) {
  GDEFSPG_Options o = pgrig::Options(pgrig::S().conninfo, p.prefix);
  GDEFSTOR_ModelBackend model;
  ASSERT_EQ(GDEFSPG_OK, gdefspg_model_backend(&o, nullptr, &model));
  p.tee.on_disagree = [seed](const std::string & what) {
    ADD_FAILURE() << "seed " << seed << ": the compiled query and the "
                  << "reference scan disagree:\n" << what;
  };
  GDEFSTOR_ModelBackend teed = tee::TeeOf(&p.tee, model);
  GDEFSTOR_Backend kv = pgrig::EmptyKv();
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_store_create_model(&kv, &teed, nullptr,
      nullptr, &p.store));
}

void Sweep(uint32_t seed, Seen & seen) {
  Pg pg;
  OpenPg(pg, seed);
  ASSERT_NE(nullptr, pg.store);
  GDEFSTOR_Store * mem = nullptr;
  ASSERT_EQ(GDEFSMEMORY_OK, gdefsmemory_store_create(nullptr, nullptr, &mem));
  Lite lite;
  ASSERT_FALSE(lite.dir.empty());
  ASSERT_EQ(GDEFSSQLITE_OK, gdefssqlite_model_store_create(lite.dir.c_str(),
      nullptr, nullptr, nullptr, &lite.store));
  /* Three generators with one seed write the same data to each store. */
  qgen::Gen g(seed);
  qgen::Gen gm(seed);
  qgen::Gen gl(seed);
  g.norders = 10 + g.Pick(20);
  gm.norders = 10 + gm.Pick(20);
  gl.norders = 10 + gl.Pick(20);
  ASSERT_EQ(GDEFSTOR_OK, g.Populate(pg.store)) << "seed " << seed;
  ASSERT_EQ(GDEFSTOR_OK, gm.Populate(mem)) << "seed " << seed;
  ASSERT_EQ(GDEFSTOR_OK, gl.Populate(lite.store)) << "seed " << seed;
  for (int n = 0; n < 150; n++) {
    qgen::Gen::Built b;
    const char * type = g.Chance(5) ? "shop.cust" : "shop.order";
    g.Make(b, type);
    std::vector<std::string> ids;
    std::vector<std::string> mids;
    std::vector<std::string> lids;
    uint64_t total = 0;
    uint64_t mtotal = 0;
    uint64_t ltotal = 0;
    GDEFSTOR_Result rc = Ask(pg.store, type, b.q, &ids, &total);
    ASSERT_EQ(GDEFSTOR_OK, rc) << "seed " << seed << " query " << n << ": "
                               << gdefstor_result_string(rc);
    ASSERT_EQ(GDEFSTOR_OK, Ask(mem, type, b.q, &mids, &mtotal));
    ASSERT_EQ(GDEFSTOR_OK, Ask(lite.store, type, b.q, &lids, &ltotal));
    EXPECT_EQ(mids, ids) << "seed " << seed << " query " << n
                         << ": Postgres and memory pages differ";
    EXPECT_EQ(lids, ids) << "seed " << seed << " query " << n
                         << ": Postgres and SQLite pages differ";
    EXPECT_EQ(mtotal, total) << "seed " << seed << " query " << n;
    EXPECT_EQ(ltotal, total) << "seed " << seed << " query " << n;
    uint64_t count = 0;
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_count(pg.store, kTenant, 2, type,
        std::strlen(type), b.q.where, &count));
    EXPECT_EQ(total, count) << "seed " << seed << " query " << n;
    seen.queries++;
    (ids.empty() ? seen.empty : seen.nonempty)++;
    seen.paged += b.q.offset > 0 && total > b.q.offset;
    seen.clamped += b.q.limit > GDEFSTOR_QUERY_LIMIT_MAX && total > 0;
    seen.partial += ids.size() < total && !ids.empty();
    seen.sorted += b.q.sort_count > 0 && ids.size() > 2;
    for (size_t k = 0; k < b.q.sort_count; k++) {
      seen.descending += b.q.sorts[k].descending && ids.size() > 2;
    }
    seen.exists += b.exists;
    seen.nots += b.nots;
    seen.field_field += b.field_field;
    seen.outer += b.outer;
  }
  EXPECT_EQ(0, pg.tee.disagreements) << pg.tee.first_disagreement;
  EXPECT_EQ(0, pg.tee.refusals);
  EXPECT_EQ(300, pg.tee.queries);
  gdefstor_store_destroy(mem);
}

TEST(PgAgreement, PostgresMatchesTheReferenceScanMemoryAndSqlite) {
  if (!pgrig::Running()) {
    GTEST_SKIP() << "SKIPPED: postgres";
  }
  Seen s;
  for (uint32_t seed = 1; seed <= 6; seed++) {
    Sweep(seed, s);
  }
  EXPECT_GT(s.nonempty, 300);
  EXPECT_GT(s.empty, 50);
  EXPECT_GT(s.paged, 50);
  EXPECT_GT(s.clamped, 20);
  EXPECT_GT(s.partial, 100);
  EXPECT_GT(s.sorted, 200);
  EXPECT_GT(s.descending, 100);
  EXPECT_GT(s.exists, 50);
  EXPECT_GT(s.nots, 100);
  EXPECT_GT(s.field_field, 20);
  EXPECT_GT(s.outer, 30);
}

/* The derived tables, emptied from outside and rebuilt by the port, hold
 * what they held, and the rebuilt store answers alike. */
TEST(PgAgreement, RebuiltCurrentTablesAnswerAlike) {
  if (!pgrig::Running()) {
    GTEST_SKIP() << "SKIPPED: postgres";
  }
  Pg pg;
  OpenPg(pg, 77);
  ASSERT_NE(nullptr, pg.store);
  qgen::Gen g(77);
  ASSERT_EQ(GDEFSTOR_OK, g.Populate(pg.store));
  uint8_t * before = nullptr;
  size_t blen = 0;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_model_dump(pg.store, kTenant, 2,
      GDEFSTOR_MODEL_CURRENT, &before, &blen));
  {
    char schema[64];
    ASSERT_EQ(GDEFSPG_OK, gdefspg_schema_name(pg.prefix.c_str(), kTenant, 2,
        schema, sizeof schema));
    pgrig::Side side(pgrig::S().conninfo);
    auto derived = side.Rows("SELECT tablename FROM pg_tables WHERE schemaname "
        "= $1 AND (tablename = 'dss_entity' OR tablename ~ '^t[0-9]+_n$' OR "
        "tablename ~ '^t[0-9]+_c[0-9]+$')", {schema});
    EXPECT_GE(derived.size(), 8u);
    for (const auto & t : derived) {
      ASSERT_TRUE(side.Exec(std::string("DELETE FROM \"") + schema + "\".\"" +
                            t[0] + "\""));
    }
  }
  ASSERT_EQ(GDEFSTOR_OK, InWrite(pg.store, [](GDEFSTOR_Write * w) {
    return gdefstor_model_rebuild(w);
  }));
  uint8_t * after = nullptr;
  size_t alen = 0;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_model_dump(pg.store, kTenant, 2,
      GDEFSTOR_MODEL_CURRENT, &after, &alen));
  EXPECT_EQ(std::string((char *)before, blen), std::string((char *)after, alen));
  gcu_allocator_free(nullptr, before);
  gcu_allocator_free(nullptr, after);
  for (int n = 0; n < 50; n++) {
    qgen::Gen::Built b;
    g.Make(b, "shop.order");
    std::vector<std::string> ids;
    uint64_t total = 0;
    ASSERT_EQ(GDEFSTOR_OK, Ask(pg.store, "shop.order", b.q, &ids, &total));
  }
  EXPECT_EQ(0, pg.tee.disagreements) << pg.tee.first_disagreement;
}

}  // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return pgrig::RunAll();
}
