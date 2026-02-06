/*
 * Copyright (C) 2024-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mf-version.h"

/**
 * mf_version_string:
 *
 * Returns: the version of the library, as a string
 */
const gchar *
mf_version_string(void)
{
    return MF_VERSION;
}
