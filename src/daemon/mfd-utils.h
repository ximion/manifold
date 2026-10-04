/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

#include "mf-utils-private.h"

G_BEGIN_DECLS

gboolean mfd_is_directory(const char *path);
gboolean mfd_is_symlink(const char *path);
gboolean mfd_is_hidden(const char *name);

gboolean mfd_remove_if_exists(const char *path,
                              GError    **error);

gboolean mfd_make_dir(const char *path,
                      GError    **error);
gboolean mfd_make_symlink(const char *target,
                          const char *path,
                          GError    **error);
gboolean mfd_replace_link(const char *target,
                          const char *dir,
                          const char *name,
                          GError    **error);

gboolean mfd_write_marker(const char *dir,
                          const char *name,
                          const char *content,
                          gssize      length,
                          GError    **error);
gboolean mfd_write_word(const char *dir,
                        const char *name,
                        const char *word,
                        GError    **error);

G_END_DECLS
