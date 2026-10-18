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
 * @file namespace.h
 * @brief Maps every public name into this build's version namespace.
 * @stability free
 */

#ifndef GHOTI_IO_GDEFSPG_NAMESPACE_H
#define GHOTI_IO_GDEFSPG_NAMESPACE_H

#include <ghoti.io/defiant-storage-postgres/libver.h>

/// @cond HIDDEN_SYMBOLS

#define GDEFSPG_Allocator GHOTIIO_DEFSPG(GDEFSPG_Allocator)
#define GDEFSPG_Result GHOTIIO_DEFSPG(GDEFSPG_Result)

#define gdefspg_allocator_default GHOTIIO_DEFSPG(gdefspg_allocator_default)
#define gdefspg_result_string GHOTIIO_DEFSPG(gdefspg_result_string)

/// @endcond

#endif /* GHOTI_IO_GDEFSPG_NAMESPACE_H */
