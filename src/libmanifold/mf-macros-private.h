/*
 * Copyright (C) 2024-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define MF_BEGIN_PRIVATE_DECLS \
    G_BEGIN_DECLS              \
    _Pragma("GCC visibility push(hidden)")

#define MF_END_PRIVATE_DECLS _Pragma("GCC visibility pop") G_END_DECLS

#define MF_INTERNAL_VISIBLE __attribute__((visibility("default")))

/* convenience functions for bitwise operations */
#define mf_flags_add(bitfield, flag) \
    do {                             \
        ((bitfield) |= (flag));      \
    } while (0)
#define mf_flags_remove(bitfield, flag) \
    do {                                \
        ((bitfield) &= ~(flag));        \
    } while (0)
#define mf_flags_invert(bitfield, flag) \
    do {                                \
        ((bitfield) ^= (flag));         \
    } while (0)
#define mf_flags_contains(bitfield, flag) (((bitfield) & (flag)) > 0)

G_END_DECLS
