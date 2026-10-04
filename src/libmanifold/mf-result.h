/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#if !defined(__MANIFOLD_H_INSIDE__) && !defined(MF_COMPILATION)
#error "Only <manifold.h> can be included directly."
#endif

#include <glib-object.h>

#include "mf-component.h"

G_BEGIN_DECLS

#define MF_TYPE_RESULT (mf_result_get_type())
G_DECLARE_DERIVABLE_TYPE(MfResult,
                         mf_result,
                         MF,
                         RESULT,
                         GObject)

struct _MfResultClass {
    GObjectClass parent_class;
    /*< private >*/
    gpointer     padding[8];
};

MfResult  *mf_result_new(void);

GPtrArray *mf_result_get_installed(MfResult *result);
GPtrArray *mf_result_get_removed(MfResult *result);
GPtrArray *mf_result_get_removed_images(MfResult *result);
guint64    mf_result_get_bytes_added(MfResult *result);
guint64    mf_result_get_bytes_freed(MfResult *result);

void       mf_result_add_installed(MfResult    *result,
                                   MfComponent *component);
void       mf_result_add_removed(MfResult    *result,
                                 MfComponent *component);
void       mf_result_set_bytes_added(MfResult *result,
                                     guint64   bytes);
void       mf_result_set_bytes_freed(MfResult *result,
                                     guint64   bytes);

G_END_DECLS
