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
 * Every public struct of this library that begins with a `size` member, once,
 * with the file that runs it (size_probe.inc, which defiant-storage installs
 * beside its conformance suites). tools/check-size-structs.sh fails, naming
 * the struct, when a header declares one that is missing here.
 */

#ifndef GHOTI_GDEFSPG_TEST_SIZE_STRUCTS_H
#define GHOTI_GDEFSPG_TEST_SIZE_STRUCTS_H

#include <ghoti.io/defiant-storage-postgres/defiant-storage-postgres.h>

#include <gtest/gtest.h>

#include "size_probe.inc"

#define GDEFSPG_SIZE_STRUCTS(X) \
  X(GDEFSPG_Options, "tests/test_pool.cpp") \
  X(GDEFSPG_StatementStats, "tests/test_pool.cpp")

namespace size_structs {

inline const std::vector<Entry> & Postgres() {
  static const std::vector<Entry> all = {
#define X(NAME, HOME) {#NAME, HOME, sizeof(NAME)},
      GDEFSPG_SIZE_STRUCTS(X)
#undef X
  };
  return all;
}

}  // namespace size_structs

#endif
