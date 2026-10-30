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

#include "mf-manifest.h"
#include "mf-signer.h"

G_BEGIN_DECLS

/**
 * MF_BUNDLE_CATALOG_DIR:
 *
 * The directory of a bundle that holds its AppStream catalog (with the
 * catalog XML, the icons and the screenshot media)
 */
#define MF_BUNDLE_CATALOG_DIR "catalog"

/**
 * MF_BUNDLE_CATALOG_ORIGIN:
 *
 * The origin of the AppStream catalog of a bundle, and of the components
 * installed from local bundles.
 */
#define MF_BUNDLE_CATALOG_ORIGIN "manifold-local"

#define MF_TYPE_BUNDLE_IMAGE (mf_bundle_image_get_type())
G_DECLARE_FINAL_TYPE(MfBundleImage, mf_bundle_image, MF, BUNDLE_IMAGE, GObject)

MfManifest *mf_bundle_image_get_manifest(MfBundleImage *image);
const char *mf_bundle_image_get_manifest_name(MfBundleImage *image);
GBytes     *mf_bundle_image_get_catalog(MfBundleImage *image);
GHashTable *mf_bundle_image_get_icons(MfBundleImage *image);

#define MF_TYPE_BUNDLE (mf_bundle_get_type())
G_DECLARE_DERIVABLE_TYPE(MfBundle, mf_bundle, MF, BUNDLE, GObject)

struct _MfBundleClass {
    GObjectClass parent_class;
    /*< private >*/
    gpointer     padding[8];
};

/**
 * MfCopyProgressFunc:
 * @bytes_copied: bytes of the image copied so far
 * @user_data: what was passed along
 *
 * Called while an image is copied, after every piece of it.
 */
typedef void (*MfCopyProgressFunc)(guint64  bytes_copied,
                                   gpointer user_data);

MfBundle      *mf_bundle_new_from_fd(int      fd,
                                     GError **error);
MfBundle      *mf_bundle_new_from_path(const char *path,
                                       GError    **error);

GPtrArray     *mf_bundle_get_images(MfBundle *bundle);
MfBundleImage *mf_bundle_find_image_for_identity(MfBundle   *bundle,
                                                 const char *identity);
gboolean       mf_bundle_is_signed(MfBundle *bundle);
gboolean       mf_bundle_sign_file(const char *path,
                                   MfSigner   *signer,
                                   GError    **error);

gboolean       mf_bundle_copy_image(MfBundle          *bundle,
                                    MfBundleImage     *image,
                                    int                dest_fd,
                                    MfCopyProgressFunc progress_func,
                                    gpointer           user_data,
                                    GError           **error);
gboolean       mf_bundle_verify_image(MfBundle          *bundle,
                                      MfBundleImage     *image,
                                      MfCopyProgressFunc progress_func,
                                      gpointer           user_data,
                                      GError           **error);

G_END_DECLS
