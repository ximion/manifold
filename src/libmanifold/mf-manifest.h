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

/**
 * MfChecksumKind:
 * @MF_CHECKSUM_KIND_UNKNOWN: Unknown checksum kind.
 * @MF_CHECKSUM_KIND_BLAKE3: BLAKE3.
 * @MF_CHECKSUM_KIND_SHA256: SHA-256.
 *
 * Kinds of checksums we support for files.
 */
typedef enum {
    MF_CHECKSUM_KIND_UNKNOWN,
    MF_CHECKSUM_KIND_BLAKE3,
    MF_CHECKSUM_KIND_SHA256,
    /*< private >*/
    MF_CHECKSUM_KIND_LAST
} MfChecksumKind;

const char    *mf_checksum_kind_to_string(MfChecksumKind kind);
MfChecksumKind mf_checksum_kind_from_string(const char *kind_str);

#define MF_TYPE_MANIFEST (mf_manifest_get_type())
G_DECLARE_DERIVABLE_TYPE(MfManifest, mf_manifest, MF, MANIFEST, GObject)

struct _MfManifestClass {
    GObjectClass parent_class;
    /*< private >*/
    gpointer     padding[8];
};

MfManifest     *mf_manifest_new_from_data(const char *data,
                                          gssize      length,
                                          GError    **error);
MfManifest     *mf_manifest_new_from_bytes(GBytes  *bytes,
                                           GError **error);
MfManifest     *mf_manifest_new_from_file(const char *path,
                                          GError    **error);

GBytes         *mf_manifest_get_data(MfManifest *manifest);

const char     *mf_manifest_get_component_id(MfManifest *manifest);
MfComponentKind mf_manifest_get_kind(MfManifest *manifest);
const char     *mf_manifest_get_version(MfManifest *manifest);
guint64         mf_manifest_get_revision(MfManifest *manifest);
const char     *mf_manifest_get_architecture(MfManifest *manifest);
gboolean        mf_manifest_is_sdk(MfManifest *manifest);
const char     *mf_manifest_get_identity(MfManifest *manifest);
const char     *mf_manifest_get_content_hash(MfManifest *manifest);

const char     *mf_manifest_get_base_id(MfManifest *manifest);
const char     *mf_manifest_get_base_version(MfManifest *manifest);
guint64         mf_manifest_get_base_revision(MfManifest *manifest);
const char     *mf_manifest_get_base_identity(MfManifest *manifest);
const char     *mf_manifest_get_base_content_hash(MfManifest *manifest);

const char     *mf_manifest_get_image_file(MfManifest *manifest);
guint64         mf_manifest_get_image_size(MfManifest *manifest);
const char     *mf_manifest_get_image_checksum(MfManifest    *manifest,
                                               MfChecksumKind kind);
const char     *mf_manifest_get_verity_hash(MfManifest *manifest);
guint64         mf_manifest_get_data_offset(MfManifest *manifest);
guint64         mf_manifest_get_data_size(MfManifest *manifest);
guint64         mf_manifest_get_hash_offset(MfManifest *manifest);
guint64         mf_manifest_get_hash_size(MfManifest *manifest);

const char     *mf_manifest_get_command(MfManifest *manifest);

G_END_DECLS
