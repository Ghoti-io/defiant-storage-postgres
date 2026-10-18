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
 * The wire helpers (src/wire_internal.h): an integer parameter's text, a
 * double's float8 bytes, a binary column read back, a tenant's schema name.
 * The first byte chooses which. Every integer's text reads back as itself and
 * holds only a sign and digits; every finite double's bytes read back to the
 * same bits; a column of any type and length is read or refused, never
 * misread; a schema name is `[a-z0-9_]`, at most 63 bytes, or refused.
 */

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "../../src/wire_internal.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 1) {
    return 0;
  }
  unsigned op = data[0] % 4;
  data++;
  size--;
  switch (op) {
  case 0: {
    int64_t v = 0;
    std::memcpy(&v, data, size < 8 ? size : 8);
    char b[24];
    size_t n = gdefspg_wire_int_text(v, b);
    if (n == 0 || n > 20 || std::strlen(b) != n) {
      std::abort();
    }
    for (size_t i = 0; i < n; i++) {
      if (!((b[i] >= '0' && b[i] <= '9') || (i == 0 && b[i] == '-'))) {
        std::abort();
      }
    }
    char * end = nullptr;
    long long back = std::strtoll(b, &end, 10);
    if (end != b + n || back != (long long)v) {
      std::abort();
    }
    break;
  }
  case 1: {
    uint64_t bits = 0;
    std::memcpy(&bits, data, size < 8 ? size : 8);
    double d;
    std::memcpy(&d, &bits, sizeof d);
    uint8_t w[8];
    gdefspg_wire_float8(d, w);
    double back = 0;
    if (gdefspg_wire_read_float8(701, w, 8, &back) != 0 ||
        std::memcmp(&back, &d, sizeof d) != 0) {
      std::abort();
    }
    break;
  }
  case 2: {
    if (size < 2) {
      return 0;
    }
    static const unsigned oids[] = {16, 17, 19, 20, 21, 23, 25, 700, 701};
    unsigned oid = oids[data[0] % 9];
    int64_t v = 0;
    double d = 0;
    int ri = gdefspg_wire_read_int(oid, data + 1, size - 1, &v);
    int rd = gdefspg_wire_read_float8(oid, data + 1, size - 1, &d);
    size_t len = size - 1;
    bool int_ok = (oid == 20 && len == 8) || (oid == 23 && len == 4) ||
                  (oid == 21 && len == 2);
    if ((ri == 0) != int_ok || (rd == 0) != (oid == 701 && len == 8)) {
      std::abort();
    }
    if (ri == 0 && oid == 21 && (v < -32768 || v > 32767)) {
      std::abort();
    }
    if (ri == 0 && oid == 23 && (v < INT32_MIN || v > INT32_MAX)) {
      std::abort();
    }
    break;
  }
  default: {
    size_t plen = size > 0 ? data[0] % 20 : 0;
    if (size < 1 + plen) {
      return 0;
    }
    char prefix[24];
    std::memcpy(prefix, data + 1, plen);
    prefix[plen] = '\0';
    char out[64];
    int rc = gdefspg_wire_schema(prefix, (const char *)data + 1 + plen,
        size - 1 - plen, out);
    if (rc == 0) {
      size_t n = std::strlen(out);
      if (n > 63 || n < 1 || out[0] < 'a' || out[0] > 'z') {
        std::abort();
      }
      for (size_t i = 0; i < n; i++) {
        char c = out[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
          std::abort();
        }
      }
    } else if (rc != -1 && rc != -2) {
      std::abort();
    }
    break;
  }
  }
  return 0;
}
