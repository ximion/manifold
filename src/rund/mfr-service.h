/*
 * Copyright (C) 2024-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib-object.h>
#include <systemd/sd-event.h>

G_BEGIN_DECLS

#define MFR_TYPE_SERVICE (mfr_service_get_type())
G_DECLARE_FINAL_TYPE(MfrService, mfr_service, MFR, SERVICE, GObject)

MfrService *mfr_service_new(sd_event *event,
                            GError  **error);

gboolean    mfr_service_listen(MfrService *service,
                               const char *socket_path,
                               GError    **error);

G_END_DECLS
