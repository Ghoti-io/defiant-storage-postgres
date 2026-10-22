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
 * The port's entity conformance suite (model.inc), the purchase-order fixture
 * included, and its key store suite (keystore.inc), on a Postgres server. Each
 * store gets schemas of its own, dropped when it closes, and its backend is
 * tee'd (model_util.h): every query the suite asks is also answered by
 * gdefstor_model_scan() over the same rows, and a difference fails the test
 * that asked it. Key stores live in a database of their own.
 */

#include <gtest/gtest.h>

#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/defiant-storage/defiant-storage.h>
#include <ghoti.io/defiant-storage-postgres/defiant-storage-postgres.h>

#include "model_util.h"
#include "pg_rig.h"

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {

struct Opened {
  std::string conninfo;
  std::string prefix;
  tee::Tee * tee = nullptr;
};

/* The allocation-failure sweeps open and close stores from several threads at
 * once (model.inc), so the table of open stores is locked. */
std::map<GDEFSTOR_Store *, Opened> & Stores() {
  static std::map<GDEFSTOR_Store *, Opened> m;
  return m;
}

std::mutex & StoresLock() {
  static std::mutex m;
  return m;
}

/* Key stores are closed by the suite itself; their schemas go at exit. */
std::vector<std::string> & KeyPrefixes() {
  static std::vector<std::string> v;
  return v;
}

void Refused(void * ctx, int result, const char * text, size_t len) {
  (void)ctx;
  ADD_FAILURE() << "a schema was refused (" << result << "): "
                << std::string(text, len);
}

}  // namespace

extern "C" GDEFSTOR_Result ModelOpen(GDEFSTOR_Store ** out,
    const GDEFSTOR_Allocator * alloc) {
  Opened o;
  o.conninfo = pgrig::ThreadConninfo();
  o.prefix = pgrig::NewPrefix();
  GDEFSPG_Options opts = pgrig::Options(o.conninfo, o.prefix);
  opts.on_refusal = Refused;
  /* A concurrent index build waits for every older transaction in the
   * database; a case that holds one write open while another commits would
   * wait the whole default for nothing. */
  opts.lock_timeout_ms = 2000;
  GDEFSTOR_ModelBackend model;
  GDEFSPG_Result rc = gdefspg_model_backend(&opts, alloc, &model);
  if (rc != GDEFSPG_OK) {
    return pgrig::ToPort(rc);
  }
  o.tee = new tee::Tee();
  o.tee->on_disagree = [](const std::string & what) {
    ADD_FAILURE() << "the compiled query and the reference scan disagree:\n"
                  << what;
  };
  GDEFSTOR_ModelBackend teed = tee::TeeOf(o.tee, model);
  GDEFSTOR_Result port = gdefstor_store_open(&teed, alloc, nullptr, out);
  if (port != GDEFSTOR_OK) {
    model.destroy(model.ctx);
    delete o.tee;
    return port;
  }
  {
    std::lock_guard<std::mutex> hold(StoresLock());
    Stores()[*out] = o;
  }
  return GDEFSTOR_OK;
}

extern "C" void ModelClose(GDEFSTOR_Store * store) {
  Opened o;
  bool known = false;
  {
    std::lock_guard<std::mutex> hold(StoresLock());
    auto it = Stores().find(store);
    if (it != Stores().end()) {
      o = it->second;
      known = true;
      Stores().erase(it);
    }
  }
  gdefstor_store_destroy(store);
  if (!known) {
    return;
  }
  EXPECT_EQ(0, o.tee->disagreements) << o.tee->first_disagreement;
  delete o.tee;
  pgrig::DropPrefix(o.conninfo, o.prefix);
}

GDEFSTOR_Result KeyStoreOpen(const GDEFSTOR_KeyCipher * cipher,
    GDEFSTOR_KeyStore ** out) {
  std::string prefix = pgrig::NewPrefix();
  KeyPrefixes().push_back(prefix);
  GDEFSPG_Options opts = pgrig::Options(pgrig::S().keys_conninfo, prefix);
  opts.on_refusal = Refused;
  return pgrig::ToPort(gdefspg_keystore_create(&opts, cipher, nullptr, out));
}

#include "model.inc"
#include "keystore.inc"

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  int rc = pgrig::RunAll();
  if (pgrig::Running()) {
    for (const std::string & p : KeyPrefixes()) {
      pgrig::DropPrefix(pgrig::S().keys_conninfo, p);
    }
  }
  return rc;
}
