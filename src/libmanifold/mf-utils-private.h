/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>
#include <systemd/sd-json.h>

#include "mf-macros-private.h"

MF_BEGIN_PRIVATE_DECLS

/* the prefix of the name of a directory or file that is not complete yet */
#define MF_TMP_PREFIX ".tmp-"

/* the suffix of AppStream MetaInfo files */
#define MF_METAINFO_SUFFIX ".metainfo.xml"

gboolean mf_write_all(int         fd,
                      const char *buffer,
                      gsize       size,
                      GError    **error);

gboolean mf_name_is_valid(const char *name,
                          const char *extra_chars,
                          size_t      length_max);

MF_INTERNAL_VISIBLE
char *mf_build_full_version(const char *version,
                            guint64     revision);
MF_INTERNAL_VISIBLE
gboolean mf_split_full_version(const char *name,
                               char      **ret_version,
                               guint64    *ret_revision);

MF_END_PRIVATE_DECLS
