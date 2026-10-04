/*
 * Copyright (C) 2024-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#if !defined(__MANIFOLD_H_INSIDE__) && !defined(MF_COMPILATION)
#error "Only <manifold.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

gboolean    mf_version_is_valid(const char *version);
int         mf_version_compare(const char *a,
                               const char *b);
int         mf_version_compare_with_rev(const char *version_a,
                                        guint64     revision_a,
                                        const char *version_b,
                                        guint64     revision_b);

gboolean    mf_hex_hash_is_valid(const char *hash);

const char *mf_get_host_architecture(void);

G_END_DECLS
