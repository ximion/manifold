/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib-object.h>

#include "mfr-app.h"

G_BEGIN_DECLS

#define MFR_IMAGES_MOUNT_DIR MF_RUN_MOUNT_DIR "/images"

#define MFR_TYPE_IMAGE_MOUNTS (mfr_image_mounts_get_type())
G_DECLARE_FINAL_TYPE(MfrImageMounts, mfr_image_mounts, MFR, IMAGE_MOUNTS, GObject)

typedef struct MfrImageMount MfrImageMount;

MfrImageMounts              *mfr_image_mounts_new(void);

MfrImageMount               *mfr_image_mounts_acquire(MfrImageMounts *mounts,
                                                      const MfrImage *image,
                                                      GError        **error);
void                         mfr_image_mounts_release(MfrImageMounts *mounts,
                                                      MfrImageMount  *img_mount);

const char                  *mfr_image_mount_get_path(const MfrImageMount *img_mount);

void                         mfr_image_mounts_remove_stale_devices(void);

G_END_DECLS
