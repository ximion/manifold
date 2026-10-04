/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <systemd/sd-json.h>

#include "mf-macros-private.h"
#include "mf-result.h"

MF_BEGIN_PRIVATE_DECLS

MfResult *mf_result_new_from_reply(sd_json_variant *reply,
                                   GError         **error);

MF_INTERNAL_VISIBLE
void mf_result_add_removed_image(MfResult   *result,
                                 const char *verity_hash);

MF_END_PRIVATE_DECLS
