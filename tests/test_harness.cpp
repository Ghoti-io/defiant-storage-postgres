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
 * What the Postgres arm reports when there is no server, which no test that
 * needs a server can check about itself. This program runs a copy of itself
 * as a Postgres test program (GDEFSPG_TEST_CHILD=1, the PgArm tests, which
 * need the server) under each condition and reads what it said:
 *
 * - with GDEFSPG_TEST_SKIP_SERVER=1 and no server: it exits 0, prints
 *   `SKIPPED: postgres`, and gtest counts one test skipped and none passed
 *   (a skip from a global environment, by contrast, gtest counts as passed);
 * - with neither: it fails, and says how to get a server;
 * - with a server that does not answer: it fails, and says so;
 * - with the server: its tests pass.
 *
 * This program itself needs no server, so it runs in every arm.
 */

#include <gtest/gtest.h>

#include "pg_rig.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

bool Child() {
  const char * c = std::getenv("GDEFSPG_TEST_CHILD");
  return c != nullptr && std::strcmp(c, "1") == 0;
}

std::string Self() {
  char b[4096];
  ssize_t n = readlink("/proc/self/exe", b, sizeof b - 1);
  if (n <= 0) {
    return std::string();
  }
  b[n] = '\0';
  return b;
}

struct Ran {
  int status = -1; /* the exit status, or -1 */
  std::string out;
};

/* Run this program as a child Postgres test program with @p env (each
 * "NAME=VALUE", or "-NAME" to unset), its stdout and stderr together. */
Ran RunChild(const std::vector<std::string> & env) {
  Ran r;
  std::string cmd = "env -u GDEFSPG_TEST_CONNINFO -u GDEFSPG_TEST_SKIP_SERVER "
                    "GDEFSPG_TEST_CHILD=1";
  for (const std::string & e : env) {
    cmd += " '" + e + "'";
  }
  cmd += " '" + Self() + "' --gtest_filter='PgArm.*' 2>&1";
  FILE * p = popen(cmd.c_str(), "r");
  if (p == nullptr) {
    return r;
  }
  std::array<char, 4096> buf;
  size_t n;
  while ((n = std::fread(buf.data(), 1, buf.size(), p)) > 0) {
    r.out.append(buf.data(), n);
  }
  int st = pclose(p);
  r.status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
  return r;
}

bool Has(const std::string & s, const std::string & what) {
  return s.find(what) != std::string::npos;
}

/* ---- the child: tests that need the server -------------------------------- */

TEST(PgArm, TheServerAnswers) {
  if (!Child()) {
    GTEST_SKIP() << "run by PgHarness only";
  }
  pgrig::Side side(pgrig::S().conninfo);
  ASSERT_TRUE(side.Ok());
  EXPECT_EQ("1", side.One("SELECT 1"));
}

TEST(PgArm, AStoreOpens) {
  if (!Child()) {
    GTEST_SKIP() << "run by PgHarness only";
  }
  std::string prefix = pgrig::NewPrefix();
  GDEFSPG_Options o = pgrig::Options(pgrig::S().conninfo, prefix);
  GDEFSTOR_Store * store = nullptr;
  ASSERT_EQ(GDEFSPG_OK, gdefspg_model_store_create(&o, nullptr, nullptr,
      &store));
  GDEFSTOR_Type * t = nullptr;
  EXPECT_EQ(GDEFSTOR_ERR_INVALID, gdefstor_type_get(store, "t1", 2, "a.b", 3,
      &t));
  gdefstor_store_destroy(store);
}

/* ---- the harness ---------------------------------------------------------- */

/* AD-25: a skipped Postgres arm is never reported as passing. */
TEST(PgHarness, ASkippedArmIsNeverReportedAsPassing) {
  Ran r = RunChild({"GDEFSPG_TEST_SKIP_SERVER=1"});
  EXPECT_EQ(0, r.status) << r.out;
  EXPECT_TRUE(Has(r.out, "SKIPPED: postgres")) << r.out;
  EXPECT_TRUE(Has(r.out, "[  SKIPPED ] 1 test")) << r.out;
  EXPECT_TRUE(Has(r.out, "[  PASSED  ] 0 tests")) << r.out;
  EXPECT_FALSE(Has(r.out, "[       OK ] PgArm")) << r.out;
}

TEST(PgHarness, NoServerFailsAndNamesTheFix) {
  Ran r = RunChild({});
  EXPECT_NE(0, r.status) << r.out;
  EXPECT_TRUE(Has(r.out, "tools/pg-test.sh")) << r.out;
  EXPECT_TRUE(Has(r.out, "GDEFSPG_TEST_SKIP_SERVER=1")) << r.out;
  EXPECT_FALSE(Has(r.out, "[       OK ] PgArm")) << r.out;
}

TEST(PgHarness, AServerThatDoesNotAnswerFails) {
  /* Port 1 on loopback: nothing listens there. */
  Ran r = RunChild({"GDEFSPG_TEST_CONNINFO=host=127.0.0.1 port=1 "
                    "connect_timeout=5"});
  EXPECT_NE(0, r.status) << r.out;
  EXPECT_TRUE(Has(r.out, "does not answer")) << r.out;
  EXPECT_FALSE(Has(r.out, "[       OK ] PgArm")) << r.out;
}

/* The arm runs when there is a server; without one, this program says what
 * the others say. */
TEST(PgHarness, WithTheServerTheArmRuns) {
  const char * ci = std::getenv("GDEFSPG_TEST_CONNINFO");
  const char * skip = std::getenv("GDEFSPG_TEST_SKIP_SERVER");
  if (ci == nullptr || ci[0] == '\0') {
    if (skip != nullptr && std::strcmp(skip, "1") == 0) {
      GTEST_SKIP() << "SKIPPED: postgres";
    }
    FAIL() << pgrig::kFix;
  }
  Ran r = RunChild({std::string("GDEFSPG_TEST_CONNINFO=") + ci});
  EXPECT_EQ(0, r.status) << r.out;
  EXPECT_TRUE(Has(r.out, "[  PASSED  ] 2 tests")) << r.out;
  EXPECT_FALSE(Has(r.out, "SKIPPED")) << r.out;
}

}  // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  if (Child()) {
    return pgrig::RunAll();
  }
  if (GTEST_FLAG_GET(filter) == "*") {
    GTEST_FLAG_SET(filter, "PgHarness.*");
  }
  return RUN_ALL_TESTS();
}
