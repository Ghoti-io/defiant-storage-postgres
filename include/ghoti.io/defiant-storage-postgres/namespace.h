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
#define GDEFSPG_IdleObserver GHOTIIO_DEFSPG(GDEFSPG_IdleObserver)
#define GDEFSPG_Options GHOTIIO_DEFSPG(GDEFSPG_Options)
#define GDEFSPG_Refusal GHOTIIO_DEFSPG(GDEFSPG_Refusal)
#define GDEFSPG_Result GHOTIIO_DEFSPG(GDEFSPG_Result)
#define GDEFSPG_StatementObserver GHOTIIO_DEFSPG(GDEFSPG_StatementObserver)
#define GDEFSPG_StatementStats GHOTIIO_DEFSPG(GDEFSPG_StatementStats)

#define gdefspg_allocator_default GHOTIIO_DEFSPG(gdefspg_allocator_default)
#define gdefspg_dialect GHOTIIO_DEFSPG(gdefspg_dialect)
#define gdefspg_keystore_create GHOTIIO_DEFSPG(gdefspg_keystore_create)
#define gdefspg_model_backend GHOTIIO_DEFSPG(gdefspg_model_backend)
#define gdefspg_model_store_create GHOTIIO_DEFSPG(gdefspg_model_store_create)
#define gdefspg_options_default GHOTIIO_DEFSPG(gdefspg_options_default)
#define gdefspg_result_string GHOTIIO_DEFSPG(gdefspg_result_string)
#define gdefspg_schema_name GHOTIIO_DEFSPG(gdefspg_schema_name)

/// @endcond

#endif /* GHOTI_IO_GDEFSPG_NAMESPACE_H */
