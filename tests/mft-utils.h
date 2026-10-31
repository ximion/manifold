/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* the components the tests make up */
#define TEST_CORE_ID         "net.debian.core13"
#define TEST_SDK_ID          "net.debian.core13.sdk"
#define TEST_PLATFORM_ID     "net.debian.core13.kde6"
#define TEST_KIT_ID          "net.debian.core13.7d225c1fe79b1c5e"      /* a kit on the core */
#define TEST_PLATFORM_KIT_ID "net.debian.core13.kde6.7d225c1fe79b1c5e" /* a kit on the platform */
#define TEST_APP_ID          "com.example.app"
#define TEST_APP_SDK_ID      "com.example.app.sdk"

char *mft_fake_hash(const char *seed);

void  mft_make_dirs(const char *path);
void  mft_make_link(const char *target,
                    const char *path);
void  mft_replace_link(const char *target,
                       const char *path);
void  mft_write_file(const char *path,
                     const char *content);
char *mft_read_file(const char *path);
void  mft_replace_in_file(const char *path,
                          const char *old,
                          const char *new);

char *mft_make_tmp_dir(const char *name);
void  mft_rm_rf(const char *path);

void  mft_run(const char        *stdin_file,
              const char *const *argv);
char *mft_make_key(const char *dir,
                   const char *name);
void  mft_check_signature(const char *dir,
                          const char *sig_namespace,
                          GBytes     *data,
                          GBytes     *signature);

void  mft_clear_store(void);

char *mft_catalog_component_xml(const char *component_id,
                                const char *ref,
                                const char *extra);
char *mft_catalog_xml(const char *components);

/**
 * MftManifest:
 *
 * What a made-up manifest records. Fields left at 0 or %NULL take the
 * default named in their comment. The identity of a component, and of its
 * base, is always the fake hash of its ID.
 */
typedef struct {
    const char *component_id;
    const char *kind;
    const char *version;
    guint64     revision; /* default: 1 */
    gboolean    sdk;
    const char *architecture; /* default: x86_64 */
    const char *content_hash; /* default: the identity */

    const char *base_id;       /* default: none */
    const char *base_version;  /* default: "1" */
    guint64     base_revision; /* default: 1 */

    const char *image_file;       /* default: "i.raw" */
    gsize       image_size;       /* default: 1 */
    const char *sha256;           /* default: the identity */
    const char *blake3;           /* default: the identity */
    const char *verity_root_hash; /* default: the identity */
    guint64     data_offset;      /* default: 20480 */
    guint64     hash_offset;      /* default: 22528 */
} MftTestManifest;

char *mft_manifest_json(const MftTestManifest *manifest);

void  mft_write_version(const char            *root,
                        const char            *subdir,
                        const MftTestManifest *manifest,
                        const char            *base_version_dir,
                        const char            *mode);

G_END_DECLS
