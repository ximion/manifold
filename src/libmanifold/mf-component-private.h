/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <systemd/sd-json.h>

#include "mf-component.h"
#include "mf-macros-private.h"

MF_BEGIN_PRIVATE_DECLS

MfComponent *mf_component_new_from_reply(sd_json_variant *variant,
                                         GError         **error);
GPtrArray   *mf_component_array_from_reply(sd_json_variant *list,
                                           GError         **error);

MF_END_PRIVATE_DECLS
