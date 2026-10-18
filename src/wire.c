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
 * Values on the wire (wire_internal.h). Integers go as text, which the
 * server reads as whatever integer type the statement gives the parameter
 * (an untyped parameter, so that a boolean bound as an int64 is read as the
 * SMALLINT its column is); everything else goes and comes back in the binary
 * format, so a double is exact and no locale or escaping is involved.
 */

#include <ghoti.io/defiant-storage-postgres/macros.h>
#include "wire_internal.h"

#include <string.h>

size_t gdefspg_wire_int_text(int64_t v, char out[24]) {
  char tmp[24];
  size_t n = 0;
  size_t i;
  /* As unsigned, so INT64_MIN negates without overflow. */
  uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
  do {
    tmp[n++] = (char)('0' + (int)(u % 10u));
    u /= 10u;
  } while (u != 0);
  i = 0;
  if (v < 0) {
    out[i++] = '-';
  }
  while (n > 0) {
    out[i++] = tmp[--n];
  }
  out[i] = '\0';
  return i;
}

void gdefspg_wire_float8(double v, uint8_t out[8]) {
  uint64_t bits;
  int k;
  memcpy(&bits, &v, sizeof bits);
  for (k = 7; k >= 0; k--) {
    out[k] = (uint8_t)(bits & 0xffu);
    bits >>= 8;
  }
}

static uint64_t be(const uint8_t * p, size_t n) {
  uint64_t v = 0;
  size_t i;
  for (i = 0; i < n; i++) {
    v = (v << 8) | p[i];
  }
  return v;
}

int gdefspg_wire_read_int(unsigned oid, const uint8_t * p, size_t len,
    int64_t * out) {
  if (p == NULL) {
    return -1;
  }
  if (oid == GDEFSPG_OID_INT8 && len == 8) {
    uint64_t u = be(p, 8);
    *out = u > (uint64_t)INT64_MAX ? -(int64_t)(~u) - 1 : (int64_t)u;
    return 0;
  }
  if (oid == GDEFSPG_OID_INT4 && len == 4) {
    uint32_t u = (uint32_t)be(p, 4);
    *out = u > 0x7fffffffu ? (int64_t)u - (int64_t)0x100000000 : (int64_t)u;
    return 0;
  }
  if (oid == GDEFSPG_OID_INT2 && len == 2) {
    uint32_t u = (uint32_t)be(p, 2);
    *out = u > 0x7fffu ? (int64_t)u - 0x10000 : (int64_t)u;
    return 0;
  }
  return -1;
}

int gdefspg_wire_read_float8(unsigned oid, const uint8_t * p, size_t len,
    double * out) {
  uint64_t bits;
  if (oid != GDEFSPG_OID_FLOAT8 || len != 8 || p == NULL) {
    return -1;
  }
  bits = be(p, 8);
  memcpy(out, &bits, sizeof *out);
  return 0;
}

int gdefspg_wire_schema(const char * prefix, const char * tenant,
    size_t tenant_len, char out[64]) {
  static const char hex[] = "0123456789abcdef";
  size_t n;
  size_t i;
  if (prefix == NULL || (tenant == NULL && tenant_len != 0)) {
    return -1;
  }
  n = strlen(prefix);
  if (n == 0 || n > 15 || prefix[0] < 'a' || prefix[0] > 'z' ||
      strncmp(prefix, "pg_", 3) == 0) {
    return -1;
  }
  for (i = 0; i < n; i++) {
    char c = prefix[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
      return -1;
    }
  }
  if (tenant_len > (63 - n) / 2) {
    return -2;
  }
  memcpy(out, prefix, n);
  for (i = 0; i < tenant_len; i++) {
    unsigned char c = (unsigned char)tenant[i];
    out[n++] = hex[c >> 4];
    out[n++] = hex[c & 0x0fu];
  }
  out[n] = '\0';
  return 0;
}
