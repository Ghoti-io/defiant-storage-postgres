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
 * Values on the wire: what a parameter is sent as and what a binary result
 * column is read from. Pure functions, apart from libpq and the server, so
 * that the fuzzer reaches them. Not installed.
 */

#ifndef GHOTI_IO_GDEFSPG_SRC_WIRE_INTERNAL_H
#define GHOTI_IO_GDEFSPG_SRC_WIRE_INTERNAL_H

#include <ghoti.io/defiant-storage-postgres/macros.h>
#include <stddef.h>
#include <stdint.h>

#define gdefspg_wire_int_text GHOTIIO_DEFSPG(gdefspg_wire_int_text)
#define gdefspg_wire_float8 GHOTIIO_DEFSPG(gdefspg_wire_float8)
#define gdefspg_wire_read_int GHOTIIO_DEFSPG(gdefspg_wire_read_int)
#define gdefspg_wire_read_float8 GHOTIIO_DEFSPG(gdefspg_wire_read_float8)
#define gdefspg_wire_schema GHOTIIO_DEFSPG(gdefspg_wire_schema)

/* Type OIDs of the built-in types a result column may have. Fixed by the
 * server's catalog (pg_type.dat) since they were assigned. */
#define GDEFSPG_OID_BYTEA 17u
#define GDEFSPG_OID_NAME 19u
#define GDEFSPG_OID_INT8 20u
#define GDEFSPG_OID_INT2 21u
#define GDEFSPG_OID_INT4 23u
#define GDEFSPG_OID_TEXT 25u
#define GDEFSPG_OID_FLOAT8 701u
#define GDEFSPG_OID_VARCHAR 1043u

/* An integer parameter in the text format: the decimal digits, with a sign,
 * and a NUL. No locale is read. Returns the length. */
GDEFSPG_INTERNAL_API size_t gdefspg_wire_int_text(int64_t v, char out[24]);

/* A double parameter in the binary format: float8, big-endian. */
GDEFSPG_INTERNAL_API void gdefspg_wire_float8(double v, uint8_t out[8]);

/* A binary integer column of type @p oid (int2, int4, int8): 0, or -1 for
 * another type or a length that is not the type's. */
GDEFSPG_INTERNAL_API int gdefspg_wire_read_int(unsigned oid,
    const uint8_t * p, size_t len, int64_t * out);

/* A binary float8 column: 0, or -1 for another type or length. */
GDEFSPG_INTERNAL_API int gdefspg_wire_read_float8(unsigned oid,
    const uint8_t * p, size_t len, double * out);

/* The schema of a tenant: @p prefix and the lowercase hex of the tenant.
 * 0, -1 for a prefix that is not `[a-z][a-z0-9_]*` of at most 15 bytes or
 * starts `pg_`, -2 when the name would pass 63 bytes. */
GDEFSPG_INTERNAL_API int gdefspg_wire_schema(const char * prefix,
    const char * tenant, size_t tenant_len, char out[64]);

#endif /* GHOTI_IO_GDEFSPG_SRC_WIRE_INTERNAL_H */
