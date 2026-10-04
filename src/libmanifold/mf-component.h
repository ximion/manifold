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

const char     *mf_component_kind_to_string(MfComponentKind kind);
MfComponentKind mf_component_kind_from_string(const char *kind_str);

gboolean        mf_component_id_is_valid(const char *component_id);

/**
 * MfInstallMode:
 * @MF_INSTALL_MODE_UNKNOWN: Not a known mode.
 * @MF_INSTALL_MODE_MANUAL: The component was installed explicitly.
 * @MF_INSTALL_MODE_AUTO: The component came along as a dependency.
 *
 * Why a component is installed, which decides whether it is a target
 * for automatic removal.
 */
typedef enum {
    MF_INSTALL_MODE_UNKNOWN,
    MF_INSTALL_MODE_MANUAL,
    MF_INSTALL_MODE_AUTO,
    /*< private >*/
    MF_INSTALL_MODE_LAST
} MfInstallMode;

const char   *mf_install_mode_to_string(MfInstallMode mode);
MfInstallMode mf_install_mode_from_string(const char *mode_str);

#define MF_TYPE_COMPONENT (mf_component_get_type())
G_DECLARE_FINAL_TYPE(MfComponent, mf_component, MF, COMPONENT, GObject)

const char     *mf_component_get_id(MfComponent *component);
MfComponentKind mf_component_get_kind(MfComponent *component);
const char     *mf_component_get_version(MfComponent *component);
guint64         mf_component_get_revision(MfComponent *component);
gboolean        mf_component_is_sdk(MfComponent *component);
const char     *mf_component_get_identity(MfComponent *component);
const char     *mf_component_get_verity_hash(MfComponent *component);
gboolean        mf_component_is_current(MfComponent *component);
MfInstallMode   mf_component_get_install_mode(MfComponent *component);
const char     *mf_component_get_origin(MfComponent *component);

G_END_DECLS
