/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* the status Meson takes as "skipped" */
#define MFT_EXIT_SKIPPED 77

void mft_sandbox_enter(void);

G_END_DECLS
