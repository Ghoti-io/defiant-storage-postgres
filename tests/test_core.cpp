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
 * Result codes: every one has a string of its own. Needs no server.
 */

#include <gtest/gtest.h>

#include <ghoti.io/defiant-storage-postgres/defiant-storage-postgres.h>

#include <set>
#include <string>

namespace {

TEST(Result, EveryResultHasAStringOfItsOwn) {
  std::set<std::string> seen;
  for (int r = 0; r < GDEFSPG_RESULT_COUNT; r++) {
    std::string s = gdefspg_result_string((GDEFSPG_Result)r);
    EXPECT_NE("Unknown error", s) << r;
    EXPECT_TRUE(seen.insert(s).second) << r << ": " << s;
  }
  EXPECT_STREQ("Unknown error", gdefspg_result_string(GDEFSPG_RESULT_COUNT));
}

TEST(Result, TheAllocatorIsCutils) {
  EXPECT_EQ(gcu_allocator_default(), gdefspg_allocator_default());
}

}  // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
