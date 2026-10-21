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

#define MF_TYPE_STORE (mf_store_get_type())
G_DECLARE_DERIVABLE_TYPE(MfStore, mf_store, MF, STORE, GObject)

struct _MfStoreClass {
    GObjectClass parent_class;
    /*< private >*/
    gpointer     padding[8];
};

MfStore     *mf_store_new(void);
MfStore     *mf_store_new_for_path(const char *path);

const char  *mf_store_get_path(MfStore *store);
gboolean     mf_store_load(MfStore *store,
                           GError **error);

GPtrArray   *mf_store_list_components(MfStore        *store,
                                      const char     *component_id,
                                      MfComponentKind kind,
                                      GError        **error);
MfComponent *mf_store_get_component(MfStore    *store,
                                    const char *component_id,
                                    const char *version,
                                    GError    **error);
MfComponent *mf_store_get_base(MfStore     *store,
                               MfComponent *component,
                               GError     **error);

char        *mf_store_get_image_path(MfStore     *store,
                                     MfComponent *component);

G_END_DECLS
