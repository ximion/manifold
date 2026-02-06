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

G_BEGIN_DECLS

/**
 * MfComponentKind:
 * @MF_COMPONENT_KIND_UNKNOWN: Not a known kind.
 * @MF_COMPONENT_KIND_CORE: The root filesystem everything else builds upon.
 * @MF_COMPONENT_KIND_PLATFORM: A platform many applications share (fixed to one core).
 * @MF_COMPONENT_KIND_KIT: The components an application needs beyond the layer below.
 * @MF_COMPONENT_KIND_APP: An application.
 * @MF_COMPONENT_KIND_PLUGIN: A plugin of an application.
 *
 * The kind of a component.
 */
typedef enum {
    MF_COMPONENT_KIND_UNKNOWN,
    MF_COMPONENT_KIND_CORE,
    MF_COMPONENT_KIND_PLATFORM,
    MF_COMPONENT_KIND_KIT,
    MF_COMPONENT_KIND_APP,
    MF_COMPONENT_KIND_PLUGIN,
    /*< private >*/
    MF_COMPONENT_KIND_LAST
} MfComponentKind;

const char                 *mf_component_kind_to_string(MfComponentKind kind);
MfComponentKind             mf_component_kind_from_string(const char *kind_str);

gboolean                    mf_component_id_is_valid(const char *component_id);

typedef struct _MfComponent MfComponent;

#define MF_TYPE_COMPONENT (mf_component_get_type())
GType        mf_component_get_type(void);

MfComponent *mf_component_copy(const MfComponent *component);
void         mf_component_free(MfComponent *component);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(MfComponent,
                              mf_component_free)

const char     *mf_component_get_id(const MfComponent *component);
MfComponentKind mf_component_get_kind(const MfComponent *component);
const char     *mf_component_get_version(const MfComponent *component);
guint64         mf_component_get_revision(const MfComponent *component);
gboolean        mf_component_is_sdk(const MfComponent *component);
const char     *mf_component_get_identity(const MfComponent *component);
const char     *mf_component_get_verity_hash(const MfComponent *component);
gboolean        mf_component_is_current(const MfComponent *component);
gboolean        mf_component_is_dependency(const MfComponent *component);
const char     *mf_component_get_origin(const MfComponent *component);

G_END_DECLS
