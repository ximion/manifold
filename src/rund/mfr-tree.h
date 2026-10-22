/*
 * Copyright (C) 2024-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib-object.h>

#include "mfr-app.h"
#include "mfr-capture.h"
#include "mfr-image-mounts.h"

G_BEGIN_DECLS

#define MFR_TREES_MOUNT_DIR MF_RUN_MOUNT_DIR "/trees"
#define MFR_TREES_CACHE_DIR MF_RUN_CACHE_DIR "/trees"

/* where the filesystem of the host is bound in every tree */
#define MFR_HOST_DIR "/run/host"

#define MFR_TYPE_TREE (mfr_tree_get_type())
G_DECLARE_FINAL_TYPE(MfrTree, mfr_tree, MFR, TREE, GObject)

MfrTree           *mfr_tree_assemble(MfrApp         *app,
                                     GHashTable     *captures,
                                     MfrImageMounts *img_mounts,
                                     GError        **error);
void               mfr_tree_teardown(MfrTree *tree);

gboolean           mfr_tree_matches(const MfrTree *tree,
                                    const MfrApp  *app);

MfrApp            *mfr_tree_get_app(const MfrTree *tree);
const char        *mfr_tree_get_key(const MfrTree *tree);
const char        *mfr_tree_get_executable(const MfrTree *tree);
const char *const *mfr_tree_get_environment(const MfrTree *tree);
gboolean           mfr_tree_clone(const MfrTree *tree,
                                  int           *ret_mount_fd,
                                  GError       **error);

GHashTable        *mfr_tree_collect_used_captures(void);
void               mfr_tree_remove_unused(void);

G_END_DECLS
