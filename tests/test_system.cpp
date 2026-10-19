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
 * Records, counters and the outbox (story S6a) where only two writers of one
 * tenant at once can show it: two stores on one tenant schema, a thread
 * each, writing at the same time as Postgres lets them (SERIALIZABLE). No
 * counter value is committed twice, and two stores never hold live leases on
 * one stream. A writer Postgres stops (a serialization failure, read as
 * GDEFSTOR_ERR_BUSY) tries again, as a caller would.
 */

#include <gtest/gtest.h>

#include "model_util.h"
#include "pg_rig.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace tu;

namespace {

struct Pair {
  std::string prefix = pgrig::NewPrefix();
  GDEFSTOR_Store * s[2] = {nullptr, nullptr};
  Pair() {
    for (int i = 0; i < 2; i++) {
      GDEFSPG_Options o = pgrig::Options(pgrig::S().conninfo, prefix);
      o.lock_timeout_ms = 10000;
      EXPECT_EQ(GDEFSPG_OK, gdefspg_model_store_create(&o, nullptr, nullptr,
          &s[i]));
    }
  }
  ~Pair() {
    gdefstor_store_destroy(s[0]);
    gdefstor_store_destroy(s[1]);
    pgrig::DropPrefix(pgrig::S().conninfo, prefix);
  }
};

uint64_t Peek(GDEFSTOR_Store * s, const char * name) {
  uint64_t v = UINT64_MAX;
  EXPECT_EQ(GDEFSTOR_OK, gdefstor_counter_peek(s, kTenant, 2, name,
      std::strlen(name), &v));
  return v;
}

/* Two stores take counter values at once, committing three writes in four
 * and aborting the fourth. Every committed value is distinct (one of two
 * writers that took one value is stopped by Postgres), and the counter ends
 * at the highest committed. What an aborted writer took is not checked here:
 * documentation/postgres.md says why it can meet a committed one. */
TEST(PgSystem, TwoStoresNeverCommitOneCounterValue) {
  Pair p;
  ASSERT_NE(nullptr, p.s[0]);
  ASSERT_NE(nullptr, p.s[1]);
  ASSERT_EQ(GDEFSTOR_OK, InWrite(p.s[0], [](GDEFSTOR_Write * w) {
    return gdefstor_record_put(w, "core.m", 6, "k", 1, nullptr, 0);
  }));
  std::mutex mu;
  std::vector<uint64_t> committed;
  std::vector<std::string> errors;
  std::atomic<int> retries{0};
  auto work = [&](int who) {
    for (int i = 0; i < 60; i++) {
      bool commit = (i + who) % 4 != 0;
      for (int attempt = 0;; attempt++) {
        GDEFSTOR_Write * w = nullptr;
        uint64_t v = 0;
        GDEFSTOR_Result rc = gdefstor_write_begin(p.s[who], kTenant, 2,
            nullptr, &w);
        if (rc == GDEFSTOR_OK) {
          rc = gdefstor_counter_next(w, "core.epoch", 10, &v);
          GDEFSTOR_Result end = rc == GDEFSTOR_OK && commit
                                    ? gdefstor_write_commit(w)
                                    : gdefstor_write_abort(w);
          rc = rc == GDEFSTOR_OK && commit ? end : rc;
        }
        if (rc == GDEFSTOR_OK) {
          if (commit) {
            std::lock_guard<std::mutex> g(mu);
            committed.push_back(v);
          }
          break;
        }
        if (rc != GDEFSTOR_ERR_BUSY || attempt > 200) {
          std::lock_guard<std::mutex> g(mu);
          errors.push_back(std::to_string(who) + ": " +
                           gdefstor_result_string(rc));
          break;
        }
        retries.fetch_add(1);
      }
    }
  };
  std::thread a(work, 0);
  std::thread b(work, 1);
  a.join();
  b.join();
  std::printf("pg counters: %zu committed, %d retries\n", committed.size(),
      retries.load());
  EXPECT_TRUE(errors.empty()) << (errors.empty() ? "" : errors[0]);
  EXPECT_EQ(90u, committed.size());
  std::set<uint64_t> unique(committed.begin(), committed.end());
  EXPECT_EQ(committed.size(), unique.size())
      << "a counter value was committed twice";
  ASSERT_FALSE(committed.empty());
  EXPECT_EQ(*std::max_element(committed.begin(), committed.end()),
      Peek(p.s[0], "core.epoch"));
}

/* Two stores claim one stream at once under hour-long leases: each row is
 * claimed once, in sequence, because a stream has one live claim. */
TEST(PgSystem, TwoStoresNeverHoldLiveLeasesOnOneStream) {
  Pair p;
  ASSERT_NE(nullptr, p.s[0]);
  ASSERT_NE(nullptr, p.s[1]);
  const int kRows = 30;
  ASSERT_EQ(GDEFSTOR_OK, InWrite(p.s[0], [&](GDEFSTOR_Write * w) {
    for (int i = 1; i <= kRows; i++) {
      std::string id = "r" + std::to_string(i);
      GDEFSTOR_Result rc = gdefstor_model_outbox_append(w, "core.bus", 8,
          id.data(), id.size(), "e/1", 3,
          reinterpret_cast<const uint8_t *>("{}"), 2, nullptr);
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
    }
    return GDEFSTOR_OK;
  }));
  std::mutex mu;
  std::vector<uint64_t> order;
  std::vector<std::string> errors;
  std::atomic<int> left{kRows};
  auto work = [&](int who) {
    int spins = 0;
    while (left.load() > 0 && spins < 20000) {
      spins++;
      GDEFSTOR_OutboxClaim * c = nullptr;
      GDEFSTOR_Result rc = gdefstor_model_outbox_claim(p.s[who], kTenant, 2,
          "core.bus", 8, 3600000, 1000, 1, &c);
      if (rc == GDEFSTOR_ERR_BUSY) {
        continue;
      }
      if (rc != GDEFSTOR_OK) {
        std::lock_guard<std::mutex> g(mu);
        errors.push_back(std::string("claim: ") + gdefstor_result_string(rc));
        return;
      }
      if (gdefstor_model_outbox_claim_count(c) == 1) {
        GDEFSTOR_OutboxEntry e;
        std::memset(&e, 0, sizeof e);
        e.size = sizeof e;
        gdefstor_model_outbox_claim_entry(c, 0, &e);
        std::string id(e.public_id, e.public_id_len);
        {
          std::lock_guard<std::mutex> g(mu);
          order.push_back(e.seq);
        }
        const char * ids[] = {id.c_str()};
        size_t lens[] = {id.size()};
        size_t gone = 0;
        for (int attempt = 0; attempt < 200; attempt++) {
          rc = gdefstor_model_outbox_delivered(p.s[who], kTenant, 2, ids, lens,
              1, &gone);
          if (rc != GDEFSTOR_ERR_BUSY) {
            break;
          }
        }
        if (rc != GDEFSTOR_OK || gone != 1) {
          std::lock_guard<std::mutex> g(mu);
          errors.push_back("delivered " + id);
        }
        left.fetch_sub(1);
      }
      gdefstor_model_outbox_claim_free(c);
    }
  };
  std::thread a(work, 0);
  std::thread b(work, 1);
  a.join();
  b.join();
  EXPECT_TRUE(errors.empty()) << (errors.empty() ? "" : errors[0]);
  ASSERT_EQ(static_cast<size_t>(kRows), order.size())
      << "a row was claimed twice, or not at all";
  for (size_t i = 0; i < order.size(); i++) {
    EXPECT_EQ(i + 1, order[i]) << "claimed out of order at " << i;
  }
}

/* A value an aborted write took stays skipped for a store opened afterwards:
 * the floor is in the tenant's schema. */
TEST(PgSystem, ASkippedCounterValueStaysSkippedForAnotherStore) {
  Pair p;
  ASSERT_NE(nullptr, p.s[0]);
  ASSERT_EQ(GDEFSTOR_OK, InWrite(p.s[0], [](GDEFSTOR_Write * w) {
    uint64_t v = 0;
    return gdefstor_counter_next(w, "core.epoch", 10, &v);
  }));
  GDEFSTOR_Write * w = nullptr;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_begin(p.s[0], kTenant, 2, nullptr, &w));
  uint64_t v = 0;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_counter_next(w, "core.epoch", 10, &v));
  EXPECT_EQ(2u, v);
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_abort(w));
  EXPECT_EQ(1u, Peek(p.s[1], "core.epoch"));
  ASSERT_EQ(GDEFSTOR_OK, InWrite(p.s[1], [](GDEFSTOR_Write * x) {
    uint64_t n = 0;
    GDEFSTOR_Result rc = gdefstor_counter_next(x, "core.epoch", 10, &n);
    return rc == GDEFSTOR_OK && n != 3 ? GDEFSTOR_ERR_INTERNAL : rc;
  }));
}

}  // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return pgrig::RunAll();
}
