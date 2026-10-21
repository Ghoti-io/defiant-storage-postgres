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
 * A tenant's snapshot (story S6a3) travels memory -> SQLite -> Postgres ->
 * memory, every truth row unchanged at each step.
 */

#include <gtest/gtest.h>

#include <ghoti.io/defiant-storage-memory/defiant-storage-memory.h>
#include <ghoti.io/defiant-storage-sqlite/defiant-storage-sqlite.h>

#include "pg_rig.h"
#include "query_gen.h"
#include "snapshot_fixture.inc"
#include "tmpdir.h"

#include <cstring>
#include <filesystem>
#include <functional>
#include <random>
#include <string>
#include <vector>

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
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_store_open(&teed, nullptr, nullptr,
      &p.store));
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
        std::strlen(type), b.q.language, b.q.language_len, b.q.where,
        &count));
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

/* ---- records, counters and the outbox (story S6a) ------------------------ */

/* The record (K), counter (C) and outbox (O) lines of a store's dump. */
std::string SysDump(GDEFSTOR_Store * s) {
  uint8_t * text = nullptr;
  size_t len = 0;
  EXPECT_EQ(GDEFSTOR_OK, gdefstor_model_dump(s, kTenant, 2,
      GDEFSTOR_MODEL_TRUTH, &text, &len));
  std::string all(reinterpret_cast<const char *>(text), len);
  gdefstor_bytes_free(nullptr, text);
  std::string out;
  size_t at = 0;
  while (at < all.size()) {
    size_t nl = all.find('\n', at);
    std::string line = all.substr(at, nl == std::string::npos ? nl : nl - at);
    if (!line.empty() && (line[0] == 'K' || line[0] == 'C' || line[0] == 'O')) {
      out += line + "\n";
    }
    at = nl == std::string::npos ? all.size() : nl + 1;
  }
  return out;
}

/* One call of the sequence, chosen once and made alike on every store. */
struct SysOp {
  int kind = 0;
  /* a write's operations: 0 put, 1 delete, 2 counter, 3 append */
  std::vector<std::pair<int, std::vector<std::string>>> ops;
  bool commit = true;
  std::string a, b;
  int64_t n1 = 0, n2 = 0;
  size_t n3 = 0;
};

/* What @p op did on @p s, as text: the results of its calls in order. */
std::string SysDo(GDEFSTOR_Store * s, const SysOp & op) {
  std::string out;
  auto rc_text = [](GDEFSTOR_Result rc) {
    return std::string(gdefstor_result_string(rc));
  };
  if (op.kind == 0) {
    GDEFSTOR_Write * w = nullptr;
    GDEFSTOR_Result rc = gdefstor_write_begin(s, kTenant, 2, nullptr, &w);
    if (rc != GDEFSTOR_OK) {
      return "begin " + rc_text(rc);
    }
    for (const auto & o : op.ops) {
      const std::vector<std::string> & v = o.second;
      if (o.first == 0) {
        rc = gdefstor_record_put(w, v[0].data(), v[0].size(), v[1].data(),
            v[1].size(), "test/1", 6, reinterpret_cast<const uint8_t *>(v[2].data()),
            v[2].size());
        out += "put " + rc_text(rc) + ";";
      } else if (o.first == 1) {
        int gone = -1;
        rc = gdefstor_record_delete(w, v[0].data(), v[0].size(), v[1].data(),
            v[1].size(), &gone);
        out += "del " + rc_text(rc) + " " + std::to_string(gone) + ";";
      } else if (o.first == 2) {
        uint64_t n = 0;
        rc = gdefstor_counter_next(w, v[0].data(), v[0].size(), &n);
        out += "next " + rc_text(rc) + " " + std::to_string(n) + ";";
      } else {
        uint64_t seq = 0;
        rc = gdefstor_model_outbox_append(w, v[0].data(), v[0].size(),
            v[1].data(), v[1].size(), "e/1", 3,
            reinterpret_cast<const uint8_t *>(v[2].data()), v[2].size(), &seq);
        out += "append " + rc_text(rc) + " " + std::to_string(seq) + ";";
      }
    }
    rc = op.commit ? gdefstor_write_commit(w) : gdefstor_write_abort(w);
    return out + (op.commit ? "commit " : "abort ") + rc_text(rc);
  }
  if (op.kind == 1) {
    int found = 0;
    uint8_t * bytes = nullptr;
    size_t len = 0;
    uint64_t n = 0;
    GDEFSTOR_Result rc = gdefstor_record_get(s, kTenant, 2, op.a.data(),
        op.a.size(), op.b.data(), op.b.size(), &found, &bytes, &len, nullptr, nullptr);
    out = "get " + rc_text(rc) + " " +
          (found ? std::string(reinterpret_cast<const char *>(bytes), len)
                 : std::string("<missing>"));
    gdefstor_bytes_free(nullptr, bytes);
    rc = gdefstor_counter_peek(s, kTenant, 2, "core.epoch", 10, &n);
    return out + " peek " + rc_text(rc) + " " + std::to_string(n);
  }
  if (op.kind == 2) {
    GDEFSTOR_RecordList * l = nullptr;
    GDEFSTOR_Result rc = gdefstor_record_list(s, kTenant, 2, op.a.data(),
        op.a.size(), op.b.data(), op.b.size(), (uint64_t)op.n1, op.n3, &l);
    out = "list " + rc_text(rc);
    for (size_t i = 0; i < gdefstor_record_list_count(l); i++) {
      const char * k = nullptr;
      size_t kn = 0;
      const uint8_t * b = nullptr;
      size_t bn = 0;
      gdefstor_record_list_item(l, i, &k, &kn, &b, &bn, nullptr, nullptr);
      out += " " + std::string(k, kn) + "=" +
             std::string(reinterpret_cast<const char *>(b), bn);
    }
    gdefstor_record_list_free(l);
    return out;
  }
  if (op.kind == 3) {
    GDEFSTOR_OutboxClaim * c = nullptr;
    GDEFSTOR_Result rc = gdefstor_model_outbox_claim(s, kTenant, 2,
        op.a.data(), op.a.size(), op.n1, op.n2, op.n3, &c);
    out = "claim " + rc_text(rc);
    for (size_t i = 0; i < gdefstor_model_outbox_claim_count(c); i++) {
      GDEFSTOR_OutboxEntry e;
      std::memset(&e, 0, sizeof e);
      e.size = sizeof e;
      gdefstor_model_outbox_claim_entry(c, i, &e);
      out += " " + std::string(e.public_id, e.public_id_len) + "@" +
             std::to_string(e.seq) + "/" + std::to_string(e.failures);
    }
    gdefstor_model_outbox_claim_free(c);
    return out;
  }
  if (op.kind == 4) {
    const char * ids[] = {op.a.c_str(), op.b.c_str()};
    size_t lens[] = {op.a.size(), op.b.size()};
    size_t gone = 0;
    GDEFSTOR_Result rc = gdefstor_model_outbox_delivered(s, kTenant, 2, ids,
        lens, 2, &gone);
    return "delivered " + rc_text(rc) + " " + std::to_string(gone);
  }
  int parked = -1;
  uint64_t n = 0;
  GDEFSTOR_Result rc = gdefstor_model_outbox_failed(s, kTenant, 2, op.a.data(),
      op.a.size(), op.n1, &parked);
  out = "failed " + rc_text(rc) + " " + std::to_string(parked);
  rc = gdefstor_model_outbox_parked(s, kTenant, 2, "", 0, &n);
  return out + " parked " + rc_text(rc) + " " + std::to_string(n);
}

/* A seeded sequence of record, counter and outbox calls, made alike on every
 * store of @p st: every answer, and at the end every record, counter and
 * outbox row, must be the same on all. */
void SystemAgree(const std::vector<GDEFSTOR_Store *> & st, uint32_t seed,
    int * out_claimed, int * out_aborts) {
  const char * const nss[] = {"core.module", "core.name"};
  const char * const keys[] = {"a", "a.b", "a:c", "b", "B", "b-1"};
  const char * const streams[] = {"core.bus", "core.log"};
  std::mt19937 rng(seed);
  auto pick = [&](size_t n) { return static_cast<size_t>(rng() % n); };
  int64_t clock = 0;
  int pids = 0;
  for (int step = 0; step < 250; step++) {
    SysOp op;
    op.kind = static_cast<int>(pick(6));
    if (op.kind == 0) {
      op.commit = pick(5) != 0;
      int n = 1 + static_cast<int>(pick(3));
      for (int i = 0; i < n; i++) {
        int k = static_cast<int>(pick(4));
        std::vector<std::string> v;
        if (k == 0) {
          v = {nss[pick(2)], keys[pick(6)], "v" + std::to_string(rng() % 100)};
        } else if (k == 1) {
          v = {nss[pick(2)], keys[pick(6)]};
        } else if (k == 2) {
          v = {pick(2) == 0 ? "core.epoch" : "core.next"};
        } else {
          v = {streams[pick(2)], "p" + std::to_string(++pids),
              std::string("{\"n\":") + std::to_string(pids) + "}"};
        }
        op.ops.push_back(std::make_pair(k, v));
      }
      *out_aborts += !op.commit;
    } else if (op.kind == 1) {
      op.a = nss[pick(2)];
      op.b = keys[pick(6)];
    } else if (op.kind == 2) {
      op.a = nss[pick(2)];
      op.b = pick(2) == 0 ? "" : std::string(keys[pick(6)], 1);
      op.n1 = static_cast<int64_t>(pick(3));
      op.n3 = 1 + pick(4);
    } else if (op.kind == 3) {
      clock += static_cast<int64_t>(pick(25));
      op.a = streams[pick(2)];
      op.n1 = 1 + static_cast<int64_t>(pick(30));
      op.n2 = clock;
      op.n3 = 1 + pick(3);
    } else if (op.kind == 4) {
      op.a = "p" + std::to_string(1 + pick(static_cast<size_t>(pids + 1)));
      op.b = "p" + std::to_string(1 + pick(static_cast<size_t>(pids + 1)));
    } else {
      op.a = "p" + std::to_string(1 + pick(static_cast<size_t>(pids + 1)));
      op.n1 = clock;
    }
    std::string first = SysDo(st[0], op);
    *out_claimed += op.kind == 3 && first.find('@') != std::string::npos;
    for (size_t k = 1; k < st.size(); k++) {
      EXPECT_EQ(first, SysDo(st[k], op)) << "seed " << seed << " step " << step
                                         << " store " << k;
    }
    if (::testing::Test::HasFailure()) {
      return;
    }
  }
  std::string dump = SysDump(st[0]);
  EXPECT_FALSE(dump.empty());
  for (size_t k = 1; k < st.size(); k++) {
    EXPECT_EQ(dump, SysDump(st[k])) << "seed " << seed << " store " << k;
  }
}

/* Memory, SQLite and Postgres give the same answer to every call of seeded
 * sequences of record, counter and outbox calls, and end holding the same
 * rows. */
TEST(PgAgreement, RecordsCountersAndTheOutboxAgreeOnEveryBackend) {
  int claimed = 0;
  int aborts = 0;
  for (uint32_t seed = 1; seed <= 4 && !HasFailure(); seed++) {
    Pg pg;
    OpenPg(pg, seed);
    ASSERT_NE(nullptr, pg.store);
    GDEFSTOR_Store * mem = nullptr;
    ASSERT_EQ(GDEFSMEMORY_OK, gdefsmemory_store_create(nullptr, nullptr, &mem));
    Lite lite;
    ASSERT_FALSE(lite.dir.empty());
    ASSERT_EQ(GDEFSSQLITE_OK, gdefssqlite_model_store_create(lite.dir.c_str(),
        nullptr, nullptr, nullptr, &lite.store));
    SystemAgree({mem, lite.store, pg.store}, seed, &claimed, &aborts);
    gdefstor_store_destroy(mem);
  }
  EXPECT_GE(claimed, 20);
  EXPECT_GE(aborts, 5);
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

/* One tenant's snapshot through every backend: memory, then SQLite, then
 * Postgres, then a second memory store. The truth dump is byte-identical at
 * every step (the source's), the current tables are rebuilt alike, and every
 * step's export is the same bytes. Once for the fixture of every kind of
 * truth, and once for each of two seeded random populations. */
void ThroughEveryBackend(
    const std::function<GDEFSTOR_Result(GDEFSTOR_Store *)> & fill,
    const std::string & what) {
  GDEFSTOR_Store * mem = nullptr;
  ASSERT_EQ(GDEFSMEMORY_OK, gdefsmemory_store_create(nullptr, nullptr, &mem));
  ASSERT_EQ(GDEFSTOR_OK, fill(mem)) << what;
  const std::string truth =
      snapshot_fixture::Dump(mem, kTenant, GDEFSTOR_MODEL_TRUTH);
  const std::string current =
      snapshot_fixture::Dump(mem, kTenant, GDEFSTOR_MODEL_CURRENT);
  ASSERT_FALSE(truth.empty()) << what;
  const std::string zip = snapshot_fixture::Export(mem, kTenant);
  ASSERT_FALSE(zip.empty()) << what;
  gdefstor_store_destroy(mem);
  std::string why;
  /* memory -> SQLite */
  Lite lite;
  ASSERT_FALSE(lite.dir.empty());
  ASSERT_EQ(GDEFSSQLITE_OK, gdefssqlite_model_store_create(lite.dir.c_str(),
      nullptr, nullptr, nullptr, &lite.store));
  ASSERT_EQ(GDEFSTOR_OK,
      snapshot_fixture::Import(lite.store, kTenant, zip, &why))
      << what << ": " << why;
  EXPECT_EQ(truth,
      snapshot_fixture::Dump(lite.store, kTenant, GDEFSTOR_MODEL_TRUTH))
      << what << ": memory to SQLite";
  EXPECT_EQ(current,
      snapshot_fixture::Dump(lite.store, kTenant, GDEFSTOR_MODEL_CURRENT))
      << what;
  const std::string from_lite = snapshot_fixture::Export(lite.store, kTenant);
  EXPECT_EQ(zip, from_lite) << what;
  /* SQLite -> Postgres */
  Pg pg;
  OpenPg(pg, 0);
  ASSERT_NE(nullptr, pg.store);
  ASSERT_EQ(GDEFSTOR_OK,
      snapshot_fixture::Import(pg.store, kTenant, from_lite, &why))
      << what << ": " << why;
  EXPECT_EQ(truth,
      snapshot_fixture::Dump(pg.store, kTenant, GDEFSTOR_MODEL_TRUTH))
      << what << ": SQLite to Postgres";
  EXPECT_EQ(current,
      snapshot_fixture::Dump(pg.store, kTenant, GDEFSTOR_MODEL_CURRENT))
      << what;
  const std::string from_pg = snapshot_fixture::Export(pg.store, kTenant);
  EXPECT_EQ(zip, from_pg) << what;
  /* Postgres -> memory */
  GDEFSTOR_Store * back = nullptr;
  ASSERT_EQ(GDEFSMEMORY_OK, gdefsmemory_store_create(nullptr, nullptr, &back));
  ASSERT_EQ(GDEFSTOR_OK, snapshot_fixture::Import(back, kTenant, from_pg, &why))
      << what << ": " << why;
  EXPECT_EQ(truth, snapshot_fixture::Dump(back, kTenant, GDEFSTOR_MODEL_TRUTH))
      << what << ": Postgres to memory";
  EXPECT_EQ(current,
      snapshot_fixture::Dump(back, kTenant, GDEFSTOR_MODEL_CURRENT))
      << what;
  EXPECT_EQ(zip, snapshot_fixture::Export(back, kTenant)) << what;
  gdefstor_store_destroy(back);
  EXPECT_EQ(0, pg.tee.disagreements) << pg.tee.first_disagreement;
}

TEST(PgAgreement, ASnapshotTravelsMemorySqlitePostgresMemoryUnchanged) {
  if (!pgrig::Running()) {
    GTEST_SKIP() << "SKIPPED: postgres";
  }
  ThroughEveryBackend([](GDEFSTOR_Store * s) {
    return snapshot_fixture::Populate(s, kTenant, false);
  }, "the fixture");
  for (uint32_t seed = 1; seed <= 2 && !HasFailure(); seed++) {
    ThroughEveryBackend([seed](GDEFSTOR_Store * s) {
      qgen::Gen g(seed);
      g.norders = 10 + g.Pick(20);
      return g.Populate(s);
    }, "seed " + std::to_string(seed));
  }
}

}  // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return pgrig::RunAll();
}
