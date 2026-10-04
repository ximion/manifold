/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib-object.h>

#include <manifold.h>

G_BEGIN_DECLS

#define MFD_TYPE_STORE_WRITER (mfd_store_writer_get_type())
G_DECLARE_FINAL_TYPE(MfdStoreWriter, mfd_store_writer, MFD, STORE_WRITER, GObject)

MfdStoreWriter *mfd_store_writer_new(MfStore *store);

gboolean        mfd_store_writer_ensure_root(MfdStoreWriter *writer,
                                             GError        **error);

gboolean        mfd_store_writer_add_image(MfdStoreWriter    *writer,
                                           MfBundle          *bundle,
                                           MfBundleImage     *image,
                                           MfCopyProgressFunc progress_func,
                                           gpointer           user_data,
                                           GError           **error);
gboolean        mfd_store_writer_remove_image(MfdStoreWriter *writer,
                                              const char     *verity_hash,
                                              GError        **error);

gboolean        mfd_store_writer_install_component(MfdStoreWriter *writer,
                                                   const char     *subdir,
                                                   const char     *component_id,
                                                   const char     *version,
                                                   guint64         revision,
                                                   const char     *verity_hash,
                                                   const char     *base_subdir,
                                                   const char     *base_component_id,
                                                   const char     *base_version,
                                                   guint64         base_revision,
                                                   GBytes         *manifest,
                                                   GBytes         *metainfo,
                                                   MfInstallMode   mode,
                                                   GError        **error);
gboolean        mfd_store_writer_remove_version_dir(MfdStoreWriter *writer,
                                                    const char     *version_dir,
                                                    GError        **error);
gboolean        mfd_store_writer_remove_cruft(MfdStoreWriter *writer,
                                              GError        **error);
gboolean        mfd_store_writer_set_mode(MfdStoreWriter *writer,
                                          const char     *version_dir,
                                          MfInstallMode   mode,
                                          GError        **error);

G_END_DECLS
