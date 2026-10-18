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
 * @file core.h
 * @brief Result codes for the Postgres storage backend.
 * @stability free
 *
 * The vocabulary matches the port's, name for name. Numeric values are not
 * a contract: a caller maps with a switch, it does not cast.
 */

#ifndef GHOTI_IO_GDEFSPG_CORE_H
#define GHOTI_IO_GDEFSPG_CORE_H

#include <ghoti.io/defiant-storage-postgres/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Result of a Postgres-backend call.
 *
 * Same names as the port, including ::GDEFSPG_ERR_BUSY, so a backend and
 * the port can be talked about with one vocabulary.
 */
typedef enum GDEFSPG_Result {
  GDEFSPG_OK = 0,          ///< Succeeded.
  GDEFSPG_ERR_IO,          ///< A stream or sink reported an error.
  GDEFSPG_ERR_FORMAT,      ///< Well formed input that is not this format.
  GDEFSPG_ERR_UNSUPPORTED, ///< Unknown format name, or a newer version.
  GDEFSPG_ERR_LIMIT,       ///< A limit was exceeded. Nothing is truncated.
  GDEFSPG_ERR_CORRUPT,     ///< This format, but the bytes are wrong.
  GDEFSPG_ERR_OOM,         ///< The allocator returned NULL.
  GDEFSPG_ERR_INVALID,     ///< A caller argument is wrong, or a missing row.
  GDEFSPG_ERR_INTERNAL,    ///< A library invariant failed.
  GDEFSPG_ERR_BUSY,        ///< A lock, a serialization conflict or a timeout: retry the call.
  GDEFSPG_RESULT_COUNT     ///< Not a result. Closes the string table.
} GDEFSPG_Result;

/**
 * @brief A static description of a result. Never NULL.
 *
 * @param result The code, including out-of-range values.
 * @return A string the caller does not free.
 */
GDEFSPG_API const char * gdefspg_result_string(GDEFSPG_Result result);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GDEFSPG_CORE_H */
