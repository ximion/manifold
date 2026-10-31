/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>
#include <zip.h>

#include "mf-bundle.h"
#include "mf-error.h"
#include "mf-macros-private.h"

MF_BEGIN_PRIVATE_DECLS

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

constexpr char MF_BUNDLE_INDEX_NAME[]      = "index.json";
constexpr char MF_BUNDLE_SIGNATURE_NAME[]  = "index.json.sig";
constexpr char MF_BUNDLE_MANIFEST_SUFFIX[] = ".manifold.json";

constexpr char MF_BUNDLE_CATALOG_PREFIX[]       = MF_BUNDLE_CATALOG_DIR "/";
constexpr char MF_BUNDLE_CATALOG_NAME[]         = MF_BUNDLE_CATALOG_DIR "/" MF_BUNDLE_CATALOG_ORIGIN ".xml";
constexpr char MF_BUNDLE_CATALOG_ICONS_PREFIX[] = MF_BUNDLE_CATALOG_DIR "/icons/";
constexpr char MF_BUNDLE_CATALOG_MEDIA_PREFIX[] = MF_BUNDLE_CATALOG_DIR "/media/";

constexpr int  MF_BUNDLE_FORMAT = 1;

gboolean       mf_bundle_file_name_is_valid(const char *name);
char          *mf_bundle_build_image_ref(MfManifest *manifest);
GBytes        *mf_bundle_get_index_data(MfBundle *bundle);

void           mf_set_error_from_zip(GError     **error,
                                     MfError      code,
                                     zip_error_t *zerr,
                                     const char  *message);

MF_END_PRIVATE_DECLS
