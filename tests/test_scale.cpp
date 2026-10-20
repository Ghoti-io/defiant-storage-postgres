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
 * The queries at 10,000 entities of one type (CAP-S7), checked by the plan
 * Postgres chooses, never by time. For each shape the SQL layer compiles on an
 * indexed field that the SQLite backend's scale test covers - equality, `in`,
 * a range, a sort over an indexed filter, the relation semi-join, the entity
 * lookup by public id, a filtered count - `EXPLAIN` of every compiled
 * statement, with the parameters it ran with, names the index the
 * declaration made. The control: a filter on a field with no index is seen
 * to scan.
 *
 * The tables are analysed after the load, as autovacuum would in time; the
 * planner is otherwise left as it is.
 *
 * GDEFSPG_SCALE_ORDERS sets another size, for looking at plans by hand.
 * GDEFSPG_SCALE_VERBOSE prints every plan.
 */

#include <gtest/gtest.h>

#include "model_util.h"
#include "pg_rig.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace tu;

namespace {

const int T = GDEFSTOR_FIELD_TEXT;
const int I = GDEFSTOR_FIELD_INTEGER;
const int R = GDEFSTOR_FIELD_REFERENCE;
const unsigned IDX = GDEFSTOR_FIELD_INDEXED;
const unsigned MUL = GDEFSTOR_FIELD_MULTIPLE;

int Orders() {
  const char * e = std::getenv("GDEFSPG_SCALE_ORDERS");
  return e != nullptr && std::atoi(e) > 0 ? std::atoi(e) : 10000;
}

const int kCusts = 1000;

std::string Id(char c, int i, int width) {
  char b[32];
  std::snprintf(b, sizeof b, "%c%0*d", c, width, i);
  return b;
}

struct Rec {
  std::string sql;
  std::vector<std::string> params; /* in the text format */
  int exec = 0;
};

std::string AsText(const GDEFSSQL_Datum & d) {
  char b[64];
  switch (d.kind) {
  case GDEFSSQL_DATUM_INT:
    std::snprintf(b, sizeof b, "%" PRId64, d.integer);
    return b;
  case GDEFSSQL_DATUM_REAL:
    std::snprintf(b, sizeof b, "%.17g", d.real);
    return b;
  case GDEFSSQL_DATUM_TEXT:
    return std::string(reinterpret_cast<const char *>(d.bytes), d.len);
  default: {
    std::string h = "\\x";
    for (size_t i = 0; i < d.len; i++) {
      std::snprintf(b, sizeof b, "%02x", d.bytes[i]);
      h += b;
    }
    return h;
  }
  }
}

void OnStatement(void * ctx, const GDEFSPG_StatementStats * s) {
  auto * v = static_cast<std::vector<Rec> *>(ctx);
  Rec r;
  r.sql.assign(s->sql, s->sql_len);
  r.exec = s->exec;
  for (size_t i = 0; i < s->bind_count; i++) {
    r.params.push_back(AsText(s->binds[i]));
  }
  v->push_back(r);
}

Node Order(int i) {
  Node n;
  n.Set("number", Txt(Id('N', i, 5)));
  n.Set("bucket", Txt(Id('b', i % 1000, 3)));
  n.Set("amount", Num(I, i));
  n.Set("buyer", Ref(Id('c', i % kCusts, 4)));
  n.Set("note", Txt("n" + std::to_string(i % 7)));
  Node & l = n.Add("lines");
  l.Set("sku", Txt(Id('s', i % 2000, 4)));
  l.Set("qty", Num(I, 1 + i % 3));
  return n;
}

Node Cust(int i) {
  Node n;
  n.Set("region", Txt(Id('r', i % 100, 2)));
  n.Set("name", Txt("name" + std::to_string(i)));
  return n;
}

GDEFSTOR_Result SaveNew(GDEFSTOR_Write * w, const char * type,
    const std::string & pid, const Node & tree) {
  GDEFSTOR_Result rc = gdefstor_entity_create(w, type, std::strlen(type),
      pid.data(), pid.size(), "", 0, 1, nullptr);
  GDEFSTOR_Tree * t = nullptr;
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_tree_create(nullptr, &t);
  }
  if (rc == GDEFSTOR_OK) {
    rc = Fill(t, 0, tree);
  }
  GDEFSTOR_RevisionMeta m = Meta();
  if (rc == GDEFSTOR_OK) {
    rc = gdefstor_entity_save(w, pid.data(), pid.size(), 0, &m, "en", 2, t,
        nullptr);
  }
  gdefstor_tree_free(t);
  return rc;
}

struct Big {
  std::string prefix = pgrig::NewPrefix();
  std::vector<Rec> recs;
  GDEFSTOR_Store * store = nullptr;
  std::unique_ptr<pgrig::Side> side;
  std::string schema;
  int orders = Orders();
  ~Big() {
    gdefstor_store_destroy(store);
    side.reset();
    pgrig::DropPrefix(pgrig::S().conninfo, prefix);
  }
};

Big * g_big = nullptr;

class Scale : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    if (!pgrig::Running()) {
      return;
    }
    g_big = new Big();
    Big & b = *g_big;
    GDEFSPG_Options o = pgrig::Options(pgrig::S().conninfo, b.prefix);
    o.on_statement = OnStatement;
    o.on_statement_ctx = &b.recs;
    ASSERT_EQ(GDEFSPG_OK, gdefspg_model_store_create(&o, nullptr, nullptr,
        &b.store));
    ASSERT_EQ(GDEFSTOR_OK, Declare(b.store, "shop.cust",
        {Field("region", T, IDX), Field("name", T)}));
    ASSERT_EQ(GDEFSTOR_OK, Declare(b.store, "shop.order",
        {Field("number", T, IDX), Field("bucket", T, IDX),
            Field("amount", I, IDX), Field("buyer", R, IDX), Field("note", T),
            Group("lines", MUL, {Field("sku", T, IDX), Field("qty", I)})}));
    const int batch = 250;
    for (int from = 0; from < kCusts; from += batch) {
      b.recs.clear();
      ASSERT_EQ(GDEFSTOR_OK, InWrite(b.store, [&](GDEFSTOR_Write * w) {
        GDEFSTOR_Result rc = GDEFSTOR_OK;
        for (int i = from; rc == GDEFSTOR_OK && i < from + batch && i < kCusts;
             i++) {
          rc = SaveNew(w, "shop.cust", Id('c', i, 4), Cust(i));
        }
        return rc;
      }));
    }
    for (int from = 0; from < b.orders; from += batch) {
      b.recs.clear();
      ASSERT_EQ(GDEFSTOR_OK, InWrite(b.store, [&](GDEFSTOR_Write * w) {
        GDEFSTOR_Result rc = GDEFSTOR_OK;
        for (int i = from; rc == GDEFSTOR_OK && i < from + batch && i < b.orders;
             i++) {
          rc = SaveNew(w, "shop.order", Id('o', i, 5), Order(i));
        }
        return rc;
      }));
    }
    b.recs.clear();
    char name[64];
    ASSERT_EQ(GDEFSPG_OK, gdefspg_schema_name(b.prefix.c_str(), kTenant, 2,
        name, sizeof name));
    b.schema = name;
    b.side.reset(new pgrig::Side(pgrig::S().conninfo));
    ASSERT_TRUE(b.side->Ok());
    /* What autovacuum would have done by now. */
    ASSERT_TRUE(b.side->Exec("ANALYZE"));
  }

  static void TearDownTestSuite() {
    delete g_big;
    g_big = nullptr;
  }

  void SetUp() override {
    if (!pgrig::Running()) {
      GTEST_SKIP() << "SKIPPED: postgres";
    }
    ASSERT_NE(nullptr, g_big);
    ASSERT_NE(nullptr, g_big->store);
    if (g_big->orders != 10000) {
      std::printf("scale: %d orders, not 10,000 (GDEFSPG_SCALE_ORDERS)\n",
          g_big->orders);
    }
  }

  std::string Tno(const char * type) {
    return g_big->side->One("SELECT tno FROM \"" + g_big->schema +
                            "\".dss_type WHERE name = $1", {type});
  }

  /* The index the declaration made for field @p path of @p type. */
  std::string IndexOf(const char * type, const char * path) {
    GDEFSTOR_Type * t = nullptr;
    EXPECT_EQ(GDEFSTOR_OK, gdefstor_type_get(g_big->store, kTenant, 2, type,
        std::strlen(type), &t));
    size_t at = 0;
    EXPECT_EQ(GDEFSTOR_OK, gdefstor_type_find(t, path, std::strlen(path), &at));
    GDEFSTOR_FieldInfo f;
    std::memset(&f, 0, sizeof f);
    f.size = sizeof f;
    EXPECT_EQ(GDEFSTOR_OK, gdefstor_type_field(t, at, &f));
    gdefstor_type_free(t);
    uint32_t group = (f.flags & GDEFSTOR_FIELD_MULTIPLE) ? f.group : f.in_group;
    uint32_t col = (f.flags & GDEFSTOR_FIELD_MULTIPLE) ? 0 : f.column;
    std::string name = g_big->side->One("SELECT name FROM \"" + g_big->schema +
        "\".dss_name WHERE role = 9 AND kind = 0 AND tno = $1::bigint AND "
        "grp = $2::bigint AND col = $3::bigint",
        {Tno(type), std::to_string(group), std::to_string(col)});
    EXPECT_NE("<none>", name) << type << "." << path << " has no built index";
    return name;
  }

  std::string EntityIndexOf(const char * type) {
    return "t" + Tno(type) + "p";
  }

  std::vector<Rec> Measure(const std::function<void()> & call) {
    g_big->recs.clear();
    call();
    return g_big->recs;
  }

  /* The compiled statements of a query: those that read the type's entity
   * table. */
  std::vector<Rec> Compiled(const std::vector<Rec> & recs, const char * type) {
    std::string e = "\"t" + Tno(type) + "_e\"";
    std::vector<Rec> out;
    for (const Rec & r : recs) {
      if (!r.exec && r.sql.rfind("SELECT", 0) == 0 &&
          r.sql.find(e) != std::string::npos) {
        out.push_back(r);
      }
    }
    return out;
  }

  std::string Plan(const Rec & r) {
    std::string out;
    for (const auto & row : g_big->side->Rows("EXPLAIN " + r.sql, r.params)) {
      out += row[0] + "\n";
    }
    if (std::getenv("GDEFSPG_SCALE_VERBOSE") != nullptr) {
      std::printf("%s\n%s\n", r.sql.c_str(), out.c_str());
    }
    return out;
  }

  static bool Uses(const std::string & plan, const std::string & ix) {
    return plan.find("Index Scan using " + ix + " ") != std::string::npos ||
           plan.find("Index Only Scan using " + ix + " ") != std::string::npos ||
           plan.find("Bitmap Index Scan on " + ix + "\n") != std::string::npos ||
           plan.find("Bitmap Index Scan on " + ix + " ") != std::string::npos;
  }

  void ExpectPlanNames(const std::vector<Rec> & recs, const char * type,
      const std::vector<std::string> & indexes) {
    std::vector<Rec> stmts = Compiled(recs, type);
    ASSERT_FALSE(stmts.empty());
    for (const Rec & r : stmts) {
      std::string plan = Plan(r);
      for (const std::string & ix : indexes) {
        EXPECT_TRUE(Uses(plan, ix)) << "the plan does not use " << ix << ":\n"
                                    << plan << "for " << r.sql;
      }
    }
  }
};

struct Q {
  std::vector<std::unique_ptr<GDEFSTOR_QueryNode>> nodes;
  std::vector<std::unique_ptr<std::vector<const GDEFSTOR_QueryNode *>>> lists;
  std::vector<std::unique_ptr<std::vector<GDEFSTOR_Value>>> vals;
  std::vector<std::unique_ptr<std::string>> strs;

  GDEFSTOR_QueryOperand Field(const char * path, int flags = 0) {
    GDEFSTOR_QueryOperand o;
    std::memset(&o, 0, sizeof o);
    o.kind = GDEFSTOR_QUERY_FIELD;
    o.field = path;
    o.field_len = std::strlen(path);
    o.flags = flags;
    return o;
  }
  GDEFSTOR_QueryOperand Entity(int flags = 0) {
    GDEFSTOR_QueryOperand o;
    std::memset(&o, 0, sizeof o);
    o.kind = GDEFSTOR_QUERY_ENTITY;
    o.flags = flags;
    return o;
  }
  GDEFSTOR_QueryOperand Values(std::vector<V> vs) {
    auto v = std::make_unique<std::vector<GDEFSTOR_Value>>();
    for (const V & x : vs) {
      strs.push_back(std::make_unique<std::string>(x.s));
      GDEFSTOR_Value g = ValueOf(x);
      g.bytes = reinterpret_cast<const uint8_t *>(strs.back()->data());
      v->push_back(g);
    }
    GDEFSTOR_QueryOperand o;
    std::memset(&o, 0, sizeof o);
    o.kind = GDEFSTOR_QUERY_VALUES;
    o.values = v->data();
    o.value_count = v->size();
    vals.push_back(std::move(v));
    return o;
  }
  const GDEFSTOR_QueryNode * Cmp(int op, GDEFSTOR_QueryOperand l,
      GDEFSTOR_QueryOperand r) {
    auto n = std::make_unique<GDEFSTOR_QueryNode>();
    std::memset(n.get(), 0, sizeof *n);
    n->op = op;
    n->left = l;
    n->right = r;
    nodes.push_back(std::move(n));
    return nodes.back().get();
  }
  const GDEFSTOR_QueryNode * Op(int op, std::vector<const GDEFSTOR_QueryNode *> kids,
      const char * exists_type = nullptr) {
    auto l = std::make_unique<std::vector<const GDEFSTOR_QueryNode *>>(kids);
    auto n = std::make_unique<GDEFSTOR_QueryNode>();
    std::memset(n.get(), 0, sizeof *n);
    n->op = op;
    n->children = l->data();
    n->child_count = l->size();
    if (exists_type != nullptr) {
      n->exists_type = exists_type;
      n->exists_type_len = std::strlen(exists_type);
    }
    lists.push_back(std::move(l));
    nodes.push_back(std::move(n));
    return nodes.back().get();
  }
};

GDEFSTOR_Query Page(const GDEFSTOR_QueryNode * where, uint64_t limit = 20) {
  GDEFSTOR_Query q;
  std::memset(&q, 0, sizeof q);
  q.size = sizeof q;
  q.language = "en";
  q.language_len = 2;
  q.where = where;
  q.limit = limit;
  return q;
}

TEST_F(Scale, EveryIndexIsBuiltAndValid) {
  for (const char * type : {"shop.cust", "shop.order"}) {
    for (const char * path : {"region", "number", "bucket", "amount", "buyer",
             "lines.sku"}) {
      if ((std::strcmp(type, "shop.cust") == 0) != (std::strcmp(path, "region") == 0)) {
        continue;
      }
      int st = -1;
      ASSERT_EQ(GDEFSTOR_OK, gdefstor_type_index_state(g_big->store, kTenant, 2,
          type, std::strlen(type), path, std::strlen(path), &st));
      EXPECT_EQ(GDEFSTOR_INDEX_READY, st) << type << "." << path;
      EXPECT_EQ("t", g_big->side->One("SELECT i.indisvalid FROM pg_index i "
          "JOIN pg_class c ON c.oid = i.indexrelid JOIN pg_namespace n ON "
          "n.oid = c.relnamespace WHERE n.nspname = $1 AND c.relname = $2",
          {g_big->schema, IndexOf(type, path)})) << type << "." << path;
    }
  }
}

TEST_F(Scale, EqualityUsesTheFieldIndex) {
  Q q;
  GDEFSTOR_Query query = Page(q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("bucket"),
      q.Values({Txt("b007")})));
  std::vector<std::string> ids;
  uint64_t total = 0;
  auto c = Measure([&]() {
    ASSERT_EQ(GDEFSTOR_OK, Ask(g_big->store, "shop.order", query, &ids, &total));
  });
  EXPECT_EQ(uint64_t(g_big->orders / 1000), total);
  ExpectPlanNames(c, "shop.order", {IndexOf("shop.order", "bucket")});
}

TEST_F(Scale, EqualityOnAMultipleGroupUsesItsIndex) {
  Q q;
  GDEFSTOR_Query query = Page(q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("lines.sku"),
      q.Values({Txt("s0042")})));
  std::vector<std::string> ids;
  uint64_t total = 0;
  auto c = Measure([&]() {
    ASSERT_EQ(GDEFSTOR_OK, Ask(g_big->store, "shop.order", query, &ids, &total));
  });
  EXPECT_EQ(uint64_t(g_big->orders / 2000), total);
  ExpectPlanNames(c, "shop.order", {IndexOf("shop.order", "lines.sku")});
}

TEST_F(Scale, InUsesTheFieldIndex) {
  Q q;
  GDEFSTOR_Query query = Page(q.Cmp(GDEFSTOR_QUERY_IN, q.Field("bucket"),
      q.Values({Txt("b001"), Txt("b002"), Txt("b003")})));
  std::vector<std::string> ids;
  uint64_t total = 0;
  auto c = Measure([&]() {
    ASSERT_EQ(GDEFSTOR_OK, Ask(g_big->store, "shop.order", query, &ids, &total));
  });
  EXPECT_EQ(uint64_t(3 * (g_big->orders / 1000)), total);
  ExpectPlanNames(c, "shop.order", {IndexOf("shop.order", "bucket")});
}

TEST_F(Scale, RangesUseTheFieldIndex) {
  for (int op : {GDEFSTOR_QUERY_GE, GDEFSTOR_QUERY_LT}) {
    Q q;
    int64_t bound = op == GDEFSTOR_QUERY_GE ? g_big->orders - 10 : 10;
    GDEFSTOR_Query query = Page(q.Cmp(op, q.Field("amount"),
        q.Values({Num(I, bound)})));
    std::vector<std::string> ids;
    uint64_t total = 0;
    auto c = Measure([&]() {
      ASSERT_EQ(GDEFSTOR_OK, Ask(g_big->store, "shop.order", query, &ids,
          &total));
    });
    EXPECT_EQ(10u, total);
    ExpectPlanNames(c, "shop.order", {IndexOf("shop.order", "amount")});
  }
}

TEST_F(Scale, ASortedFilteredPageUsesTheFilterIndex) {
  Q q;
  GDEFSTOR_Query query = Page(q.Cmp(GDEFSTOR_QUERY_IN, q.Field("bucket"),
      q.Values({Txt("b001"), Txt("b002"), Txt("b003"), Txt("b004"),
          Txt("b005")})));
  GDEFSTOR_QuerySort s;
  std::memset(&s, 0, sizeof s);
  s.key = q.Field("amount");
  s.descending = 1;
  query.sorts = &s;
  query.sort_count = 1;
  std::vector<std::string> ids;
  uint64_t total = 0;
  auto c = Measure([&]() {
    ASSERT_EQ(GDEFSTOR_OK, Ask(g_big->store, "shop.order", query, &ids, &total));
  });
  uint64_t want = 5 * (uint64_t)(g_big->orders / 1000);
  EXPECT_EQ(want, total);
  ASSERT_EQ(std::min<size_t>(20, want), ids.size());
  int last = g_big->orders - 1;
  while (last % 1000 < 1 || last % 1000 > 5) {
    last--;
  }
  EXPECT_EQ(Id('o', last, 5), ids[0]);
  ExpectPlanNames(c, "shop.order", {IndexOf("shop.order", "bucket")});
}

TEST_F(Scale, TheRelationSemiJoinUsesBothIndexes) {
  Q q;
  const GDEFSTOR_QueryNode * exists = q.Op(GDEFSTOR_QUERY_EXISTS,
      {q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("buyer"),
          q.Entity(GDEFSTOR_QUERY_OUTER))},
      "shop.order");
  GDEFSTOR_Query query = Page(q.Op(GDEFSTOR_QUERY_AND,
      {q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("region"), q.Values({Txt("r07")})),
          exists}));
  std::vector<std::string> ids;
  uint64_t total = 0;
  auto c = Measure([&]() {
    ASSERT_EQ(GDEFSTOR_OK, Ask(g_big->store, "shop.cust", query, &ids, &total));
  });
  uint64_t want = g_big->orders >= kCusts ? kCusts / 100 : 0;
  EXPECT_EQ(want, total);
  ExpectPlanNames(c, "shop.cust", {IndexOf("shop.cust", "region"),
      IndexOf("shop.order", "buyer")});
}

TEST_F(Scale, TheEntityLookupByPublicIdUsesItsIndex) {
  Q q;
  std::string pid = Id('o', g_big->orders / 2, 5);
  GDEFSTOR_Query query = Page(q.Cmp(GDEFSTOR_QUERY_EQ, q.Entity(),
      q.Values({Ref(pid)})));
  std::vector<std::string> ids;
  uint64_t total = 0;
  auto c = Measure([&]() {
    ASSERT_EQ(GDEFSTOR_OK, Ask(g_big->store, "shop.order", query, &ids, &total));
  });
  EXPECT_EQ(1u, total);
  ASSERT_EQ(1u, ids.size());
  EXPECT_EQ(pid, ids[0]);
  ExpectPlanNames(c, "shop.order", {EntityIndexOf("shop.order")});
}

TEST_F(Scale, AFilteredCountUsesTheFieldIndex) {
  Q q;
  uint64_t n = 0;
  const GDEFSTOR_QueryNode * where = q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("bucket"),
      q.Values({Txt("b123")}));
  auto c = Measure([&]() {
    ASSERT_EQ(GDEFSTOR_OK, gdefstor_entity_count(g_big->store, kTenant, 2,
        "shop.order", 10, "en", 2, where, &n));
  });
  EXPECT_EQ(uint64_t(g_big->orders / 1000), n);
  ExpectPlanNames(c, "shop.order", {IndexOf("shop.order", "bucket")});
}

/* The control: a filter on a field with no index is seen to be applied row
 * by row (a Filter, never an Index Cond on its column), so a plan that names
 * an index above is a measurement and not a blind spot. */
TEST_F(Scale, AFilterOnAFieldWithNoIndexIsSeenToFilterRows) {
  Q q;
  GDEFSTOR_Query query = Page(q.Cmp(GDEFSTOR_QUERY_EQ, q.Field("note"),
      q.Values({Txt("n3")})));
  std::vector<std::string> ids;
  uint64_t total = 0;
  auto c = Measure([&]() {
    ASSERT_EQ(GDEFSTOR_OK, Ask(g_big->store, "shop.order", query, &ids, &total));
  });
  EXPECT_GT(total, 0u);
  GDEFSTOR_Type * t = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_type_get(g_big->store, kTenant, 2,
      "shop.order", 10, &t));
  size_t at = 0;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_type_find(t, "note", 4, &at));
  GDEFSTOR_FieldInfo f;
  std::memset(&f, 0, sizeof f);
  f.size = sizeof f;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_type_field(t, at, &f));
  gdefstor_type_free(t);
  std::string col = "v" + std::to_string(f.column);
  std::vector<Rec> stmts = Compiled(c, "shop.order");
  ASSERT_FALSE(stmts.empty());
  for (const Rec & r : stmts) {
    std::string plan = Plan(r);
    /* The filter names the query's language too: `(lang = ...) AND (vN = ...)`. */
    EXPECT_NE(std::string::npos, plan.find("Filter: (")) << plan;
    EXPECT_NE(std::string::npos, plan.find("(" + col + " = ")) << plan;
    EXPECT_EQ(std::string::npos, plan.find("Index Cond: (" + col + " ")) << plan;
  }
  /* And an indexed field's filter is an Index Cond on its column. */
  Q k;
  GDEFSTOR_Query indexed = Page(k.Cmp(GDEFSTOR_QUERY_EQ, k.Field("bucket"),
      k.Values({Txt("b007")})));
  auto ci = Measure([&]() {
    ASSERT_EQ(GDEFSTOR_OK, Ask(g_big->store, "shop.order", indexed, &ids,
        &total));
  });
  bool cond = false;
  for (const Rec & r : Compiled(ci, "shop.order")) {
    cond |= Plan(r).find("Index Cond: (") != std::string::npos;
  }
  EXPECT_TRUE(cond);
}

}  // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return pgrig::RunAll();
}
