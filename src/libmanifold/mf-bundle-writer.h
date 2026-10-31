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

#include "mf-signer.h"

G_BEGIN_DECLS

#define MF_TYPE_BUNDLE_WRITER (mf_bundle_writer_get_type())
G_DECLARE_DERIVABLE_TYPE(MfBundleWriter, mf_bundle_writer, MF, BUNDLE_WRITER, GObject)

struct _MfBundleWriterClass {
    GObjectClass parent_class;
    /*< private >*/
    gpointer     padding[8];
};

MfBundleWriter *mf_bundle_writer_new(const char *path,
                                     GError    **error);
MfBundleWriter *mf_bundle_writer_new_for_file(const char *path,
                                              GError    **error);

gboolean        mf_bundle_writer_add_image(MfBundleWriter *writer,
                                           const char     *image_path,
                                           GBytes         *manifest,
                                           GBytes         *catalog,
                                           GError        **error);
gboolean        mf_bundle_writer_add_catalog_assets(MfBundleWriter *writer,
                                                    const char     *icons_dir,
                                                    const char     *media_dir,
                                                    GError        **error);

gboolean        mf_bundle_writer_finish(MfBundleWriter *writer,
                                        MfSigner       *signer,
                                        GError        **error);

G_END_DECLS
