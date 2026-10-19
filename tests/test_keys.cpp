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
 * The key store on Postgres after an erasure (story S5b). A deleted row stays
 * in its table's pages as a dead tuple until a vacuum, so the dialect's
 * key_vacuum (`VACUUM (FULL) `) runs on dsk_key and dsk_sealed after an
 * erasure commits, outside any transaction, and writes each table anew: the
 * table's file is replaced, so the old pages, with the key in them, go. The
 * write-ahead log and its archives are outside this library's reach.
 */

#include <gtest/gtest.h>

#include "pg_rig.h"

#include <ghoti.io/cutil/allocator.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

struct Rec {
  std::string sql;
  int in_txn = 0;
  int failed = 0;
};

void OnStatement(void * ctx, const GDEFSPG_StatementStats * s) {
  Rec r;
  r.sql.assign(s->sql, s->sql_len);
  r.in_txn = s->in_transaction;
  r.failed = s->failed;
  static_cast<std::vector<Rec> *>(ctx)->push_back(r);
}

/* NOT A CIPHER: the key store needs one to make keys; nothing here opens. */
uint64_t g_counter = 0x9e3779b97f4a7c15ull;
GDEFSTOR_Result Random(void *, uint8_t * out, size_t n) {
  for (size_t i = 0; i < n; i++) {
    g_counter = g_counter * 6364136223846793005ull + 1442695040888963407ull;
    out[i] = static_cast<uint8_t>(g_counter >> 56);
  }
  return GDEFSTOR_OK;
}
GDEFSTOR_Result Copy(void *, const GDEFSTOR_Allocator * alloc, const uint8_t *,
    const uint8_t * in, size_t n, uint8_t ** out, size_t * out_len) {
  uint8_t * o = static_cast<uint8_t *>(gcu_allocator_malloc(alloc, n + 1));
  if (o == nullptr) {
    return GDEFSTOR_ERR_OOM;
  }
  if (n > 0) {
    std::memcpy(o, in, n);
  }
  *out = o;
  *out_len = n;
  return GDEFSTOR_OK;
}

class PgKeys : public ::testing::Test {
 protected:
  std::vector<Rec> recs;
  std::string prefix = pgrig::NewPrefix();
  GDEFSTOR_KeyStore * ks = nullptr;
  GDEFSTOR_KeyCipher cipher;
  std::string schema;

  void SetUp() override {
    if (!pgrig::Running()) {
      GTEST_SKIP() << "SKIPPED: postgres";
    }
    std::memset(&cipher, 0, sizeof cipher);
    cipher.size = sizeof cipher;
    cipher.key_bytes = 32;
    cipher.random = Random;
    cipher.seal = Copy;
    cipher.open = Copy;
    GDEFSPG_Options o = pgrig::Options(pgrig::S().keys_conninfo, prefix);
    o.on_statement = OnStatement;
    o.on_statement_ctx = &recs;
    ASSERT_EQ(GDEFSPG_OK, gdefspg_keystore_create(&o, &cipher, nullptr, &ks));
    char b[64];
    ASSERT_EQ(GDEFSPG_OK, gdefspg_schema_name(prefix.c_str(), "t1", 2, b,
        sizeof b));
    schema = b;
  }
  void TearDown() override {
    gdefstor_keystore_destroy(ks);
    if (pgrig::Running()) {
      pgrig::DropPrefix(pgrig::S().keys_conninfo, prefix);
    }
  }
  std::string FileOf(pgrig::Side & side, const char * table) {
    return side.One("SELECT pg_relation_filenode(c.oid)::text FROM pg_class c "
                    "JOIN pg_namespace n ON n.oid = c.relnamespace WHERE "
                    "n.nspname = $1 AND c.relname = $2",
        {schema, table});
  }
  int Vacuums(const char * table) {
    int n = 0;
    for (const Rec & r : recs) {
      if (r.sql.rfind("VACUUM (FULL) ", 0) == 0 &&
          r.sql.find(table) != std::string::npos) {
        EXPECT_FALSE(r.in_txn) << r.sql;
        EXPECT_FALSE(r.failed) << r.sql;
        n++;
      }
    }
    return n;
  }
};

/* An erasure that wrote its ledger row vacuums both tables a key can be in,
 * outside the transaction, and each table's file is replaced; erasing the
 * subject again writes nothing and vacuums nothing. */
TEST_F(PgKeys, AnErasureVacuumsTheKeyTablesOutsideTheTransaction) {
  const uint8_t m[4] = {1, 2, 3, 4};
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_subject_create(ks, "t1", 2, "s1", 2));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_subject_put(ks, "t1", 2, "s2", 2, m,
      4));
  pgrig::Side side(pgrig::S().keys_conninfo);
  ASSERT_TRUE(side.Ok());
  std::string key_file = FileOf(side, "dsk_key");
  std::string sealed_file = FileOf(side, "dsk_sealed");
  ASSERT_FALSE(key_file.empty());
  ASSERT_FALSE(sealed_file.empty());
  EXPECT_EQ(0, Vacuums("dsk_key"));
  recs.clear();
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_erase(ks, "t1", 2, "s1", 2, 7));
  EXPECT_EQ(1, Vacuums("\"dsk_key\""));
  EXPECT_EQ(1, Vacuums("\"dsk_sealed\""));
  EXPECT_NE(key_file, FileOf(side, "dsk_key")) << "dsk_key was not rewritten";
  EXPECT_NE(sealed_file, FileOf(side, "dsk_sealed"));
  EXPECT_EQ("0", side.One("SELECT count(*)::text FROM \"" + schema +
                          "\".dsk_key"));
  recs.clear();
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_erase(ks, "t1", 2, "s1", 2, 8));
  EXPECT_EQ(0, Vacuums("dsk_"));
  /* A restore whose ledger erases a subject vacuums the same way. */
  recs.clear();
  const std::string set = "defiant-keyset/1\ntaken 9\nledger s2 9\n";
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_keystore_import(ks, "t1", 2,
      reinterpret_cast<const uint8_t *>(set.data()), set.size()));
  EXPECT_EQ(1, Vacuums("\"dsk_sealed\""));
  EXPECT_EQ("0", side.One("SELECT count(*)::text FROM \"" + schema +
                          "\".dsk_sealed"));
}

}  // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return pgrig::RunAll();
}
