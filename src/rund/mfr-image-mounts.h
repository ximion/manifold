/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

#include "mfr-app.h"

G_BEGIN_DECLS

#define MFR_IMAGES_MOUNT_DIR MF_RUN_MOUNT_DIR "/images"

typedef struct MfrImageMounts MfrImageMounts;

MfrImageMounts               *mfr_image_mounts_new(void);
void                          mfr_image_mounts_free(MfrImageMounts *mounts);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(MfrImageMounts,
                              mfr_image_mounts_free)

const char *mfr_image_mounts_acquire(MfrImageMounts *mounts,
                                     const MfrLayer *layer,
                                     GError        **error);
void        mfr_image_mounts_release(MfrImageMounts *mounts,
                                     const char     *verity_hash);

G_END_DECLS
