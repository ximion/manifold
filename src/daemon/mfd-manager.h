/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib-object.h>

#include <manifold.h>

G_BEGIN_DECLS

/**
 * MfdErrorDetail:
 * @component_id: the component the error is about
 * @version: the version of that component, for %MF_ERROR_ALREADY_INSTALLED
 * @base_id: the missing base, for %MF_ERROR_MISSING_BASE
 * @base_version: its version, for %MF_ERROR_MISSING_BASE
 * @identity: its identity, for %MF_ERROR_MISSING_BASE
 * @used_by: a component that needs @component_id, for %MF_ERROR_COMPONENT_IN_USE
 * @architecture: what @component_id was built for, for %MF_ERROR_WRONG_ARCHITECTURE
 * @kind: the kind of what is installed with @component_id, for %MF_ERROR_ID_CONFLICT
 * @sdk: whether that is an SDK, for %MF_ERROR_ID_CONFLICT
 *
 * Details about the failure of an operation, beyond the #GError.
 */
typedef struct {
    char *component_id;
    char *version;
    char *base_id;
    char *base_version;
    char *identity;
    char *used_by;
    char *architecture;
    MfComponentKind kind;
    gboolean sdk;
} MfdErrorDetail;

void mfd_error_detail_clear(MfdErrorDetail *detail);
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(MfdErrorDetail,
                                 mfd_error_detail_clear)

/**
 * MfdRemovalItem:
 * @component: a version of a component that will be deleted
 * @action: why it is deleted: %MF_ACTION_REMOVE if it was asked for,
 *   %MF_ACTION_CLEANUP if nothing needs it anymore
 *
 * One version a removal deletes.
 */
typedef struct {
    MfComponent *component;
    MfAction     action;
} MfdRemovalItem;

/**
 * MfdRemovalPlan:
 * @items: (element-type MfdRemovalItem): the versions that will be deleted, in removal order
 * @cpts_set: (element-type MfComponent): the components of @items, as a deduplication set
 * @images: (element-type utf8): the root hashes of the images that will be removed
 * @image_bytes: how much disk space the images take up
 *
 * What a removal will remove, or has removed, and the state needed to apply it.
 */
typedef struct {
    GPtrArray  *items;
    GHashTable *cpts_set;
    GPtrArray  *images;
    guint64     image_bytes;
} MfdRemovalPlan;

void mfd_removal_plan_free(MfdRemovalPlan *rmplan);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(MfdRemovalPlan,
                              mfd_removal_plan_free)

#define MFD_TYPE_MANAGER (mfd_manager_get_type())
G_DECLARE_FINAL_TYPE(MfdManager, mfd_manager, MFD, MANAGER, GObject)

MfdManager     *mfd_manager_new(void);
MfdManager     *mfd_manager_new_for_store(MfStore *store);

MfResult       *mfd_manager_install_bundle(MfdManager     *manager,
                                           MfBundle       *bundle,
                                           MfProgressFunc  progress_func,
                                           gpointer        user_data,
                                           MfdErrorDetail *ret_detail,
                                           GError        **error);

MfdRemovalPlan *mfd_manager_plan_removal(MfdManager     *manager,
                                         const char     *component_id,
                                         const char     *version,
                                         MfdErrorDetail *ret_detail,
                                         GError        **error);
MfResult       *mfd_manager_remove(MfdManager     *manager,
                                   const char     *component_id,
                                   const char     *version,
                                   MfProgressFunc  progress_func,
                                   gpointer        user_data,
                                   MfdErrorDetail *ret_detail,
                                   GError        **error);

GPtrArray      *mfd_manager_mark(MfdManager     *manager,
                                 const char     *component_id,
                                 const char     *version,
                                 MfInstallMode   mode,
                                 MfdErrorDetail *ret_detail,
                                 GError        **error);

G_END_DECLS
