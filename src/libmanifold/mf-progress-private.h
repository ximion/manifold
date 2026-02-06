/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <systemd/sd-json.h>

#include "mf-macros-private.h"
#include "mf-progress.h"

MF_BEGIN_PRIVATE_DECLS

MfProgress *mf_progress_new_from_reply(sd_json_variant *variant);

MF_END_PRIVATE_DECLS
