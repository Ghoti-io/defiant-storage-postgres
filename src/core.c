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
 * Result strings and the default allocator.
 */

#include <ghoti.io/defiant-storage-postgres/allocator.h>
#include <ghoti.io/defiant-storage-postgres/core.h>
#include <ghoti.io/cutil/allocator.h>

const GDEFSPG_Allocator * gdefspg_allocator_default(void) {
  return gcu_allocator_default();
}

const char * gdefspg_result_string(GDEFSPG_Result result) {
  switch (result) {
  case GDEFSPG_OK:
    return "No error";
  case GDEFSPG_ERR_IO:
    return "I/O error";
  case GDEFSPG_ERR_FORMAT:
    return "Invalid format";
  case GDEFSPG_ERR_UNSUPPORTED:
    return "Unsupported format";
  case GDEFSPG_ERR_LIMIT:
    return "Limit exceeded";
  case GDEFSPG_ERR_CORRUPT:
    return "Corrupt data";
  case GDEFSPG_ERR_OOM:
    return "Out of memory";
  case GDEFSPG_ERR_INVALID:
    return "Invalid argument";
  case GDEFSPG_ERR_INTERNAL:
    return "Internal error";
  case GDEFSPG_ERR_BUSY:
    return "Database busy";
  case GDEFSPG_RESULT_COUNT:
  default:
    return "Unknown error";
  }
}
