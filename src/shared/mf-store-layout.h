/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "config.h"

#include <glib.h>

G_BEGIN_DECLS

#define MF_STORE_IMAGES_DIR   MF_STORE_DIR "/images/by-verity"
#define MF_STORE_APPS_DIR     MF_STORE_DIR "/apps"
#define MF_STORE_PLUGINS_DIR  MF_STORE_DIR "/plugins"
#define MF_STORE_RUNTIMES_DIR MF_STORE_DIR "/runtimes"

#define MF_STORE_COMPONENT_TO_ROOT   "../../.."
#define MF_STORE_COMPONENT_TO_IMAGES MF_STORE_COMPONENT_TO_ROOT "/images/by-verity"

#define MF_STORE_IMAGE_SUFFIX       ".raw"
#define MF_STORE_APP_IMAGE_LINK     "app.raw"
#define MF_STORE_RUNTIME_IMAGE_LINK "runtime.raw"
#define MF_STORE_BASE_LINK          "base"
#define MF_STORE_MANIFEST_NAME      "manifest.json"
#define MF_STORE_CURRENT_NAME       "current"

/* the length of a hex encoded 256 bit hash, which names images */
#define MF_HEX_HASH_LENGTH 64

/**
 * mf_hex_hash_is_valid:
 * @hash: a string of lowercase hex digits
 *
 * We use hashes in file and directory names.
 * This function validates external hashes, only lowercase hex of the
 * length of a 256 bit hash is accepted.
 *
 * Returns: %TRUE if @hash is a valid hex hash, %FALSE otherwise
 */
static inline gboolean
mf_hex_hash_is_valid(const char *hash)
{
    if (hash == NULL || strnlen(hash, MF_HEX_HASH_LENGTH + 1) != MF_HEX_HASH_LENGTH)
        return FALSE;

    for (size_t i = 0; i < MF_HEX_HASH_LENGTH; i++) {
        if (!g_ascii_isxdigit(hash[i]) || g_ascii_isupper(hash[i]))
            return FALSE;
    }

    return TRUE;
}

G_END_DECLS
