/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>
#include <systemd/sd-event.h>
#include <systemd/sd-json.h>

G_DEFINE_AUTOPTR_CLEANUP_FUNC(sd_event,
                              sd_event_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(sd_json_variant,
                              sd_json_variant_unref)
