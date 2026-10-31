/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "config.h"

#include <glib.h>

#include "mf-store.h"
#include "mf-bundle-private.h"
#include "mf-macros-private.h"

MF_BEGIN_PRIVATE_DECLS

#define MF_STORE_IMAGES_SUBDIR   "images/by-verity"
#define MF_STORE_APPS_SUBDIR     "apps"
#define MF_STORE_PLUGINS_SUBDIR  "plugins"
#define MF_STORE_RUNTIMES_SUBDIR "runtimes"

#define MF_STORE_IMAGES_DIR   MF_STORAGE_DIR "/" MF_STORE_IMAGES_SUBDIR
#define MF_STORE_APPS_DIR     MF_STORAGE_DIR "/" MF_STORE_APPS_SUBDIR
#define MF_STORE_RUNTIMES_DIR MF_STORAGE_DIR "/" MF_STORE_RUNTIMES_SUBDIR

/* the AppStream catalog of what was installed from bundles, a file and icons per component */
#define MF_STORE_LOCAL_CATALOG_SUBDIR "origins/local/catalog"
#define MF_STORE_LOCAL_ICONS_SUBDIR   "origins/local/icons/" MF_BUNDLE_CATALOG_ORIGIN

#define MF_STORE_COMPONENT_TO_ROOT     "../../.."
#define MF_STORE_LOCAL_CATALOG_TO_ROOT "../../.."
#define MF_STORE_LOCAL_ICONS_TO_ROOT   "../../../../.."
#define MF_STORE_COMPONENT_TO_IMAGES   MF_STORE_COMPONENT_TO_ROOT "/images/by-verity"

#define MF_STORE_IMAGE_SUFFIX       ".raw"
#define MF_STORE_APP_IMAGE_LINK     "app.raw"
#define MF_STORE_RUNTIME_IMAGE_LINK "runtime.raw"
#define MF_STORE_BASE_LINK          "base"
#define MF_STORE_MANIFEST_NAME      "manifest.json"
#define MF_STORE_CURRENT_NAME       "current"
#define MF_STORE_ORIGIN_NAME        "origin"
#define MF_STORE_MODE_NAME          "mode"
#define MF_STORE_ICONS_NAME         "icons"
#define MF_STORE_CATALOG_SUFFIX     ".xml"

#define MF_STORE_REVOKED_NAME "revoked"
#define MF_STORE_LOCAL_ORIGIN "local"

/* the layout of the store */
MF_INTERNAL_VISIBLE
const char *const *mf_store_get_component_subdirs(void);
MF_INTERNAL_VISIBLE
const char *mf_store_subdir_for_kind(MfComponentKind kind,
                                     gboolean        sdk);
MF_INTERNAL_VISIBLE
const char *mf_store_image_link_for_subdir(const char *subdir);
MF_INTERNAL_VISIBLE
char *mf_store_build_image_name(const char *verity_hash);
MF_INTERNAL_VISIBLE
char *mf_store_hash_from_image_name(const char *name);
MF_INTERNAL_VISIBLE
void mf_store_invalidate(MfStore *store);
MF_INTERNAL_VISIBLE
gboolean mf_store_has_image(MfStore    *store,
                            const char *verity_hash);
MF_INTERNAL_VISIBLE
GPtrArray *mf_store_list_images(MfStore *store,
                                GError **error);
MF_INTERNAL_VISIBLE
MfComponent *mf_store_find_identity(MfStore    *store,
                                    const char *identity,
                                    GError    **error);

/**
 * MfBaseLink:
 * @subdir: the subdirectory of the store the base is in
 * @component_id: the identifier of the base
 * @version_name: the version directory of the base the link names,
 *   `<version>-<revision>`, or %NULL if it follows the current version
 *
 * What the `base` link of a version of a component names.
 */
typedef struct {
    char *subdir;
    char *component_id;
    char *version_name;
} MfBaseLink;

MF_INTERNAL_VISIBLE
MfBaseLink *mf_base_link_new(const char *subdir,
                             const char *component_id,
                             const char *version_name);
MF_INTERNAL_VISIBLE
void mf_base_link_free(MfBaseLink *link);
MF_INTERNAL_VISIBLE
char *mf_base_link_get_target(const MfBaseLink *link);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MfBaseLink, mf_base_link_free)

/* direct access to one store root, without loading it */
MF_INTERNAL_VISIBLE
MfComponent *mf_store_read_component_at(const char *root,
                                        const char *component_id,
                                        const char *version,
                                        GError    **error);
MF_INTERNAL_VISIBLE
MfComponent *mf_store_read_base_at(const char  *root,
                                   MfComponent *component,
                                   GError     **error);
MF_INTERNAL_VISIBLE
MfBaseLink *mf_store_read_base_link(MfComponent *component,
                                    GError     **error);
MF_INTERNAL_VISIBLE
gboolean mf_store_component_is_revoked_at(const char *root,
                                          const char *component_id);
MF_INTERNAL_VISIBLE
char *mf_store_image_path_at(const char *root,
                             const char *verity_hash);

MF_END_PRIVATE_DECLS
