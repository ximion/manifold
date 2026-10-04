/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib-object.h>
#include <systemd/sd-event.h>

G_BEGIN_DECLS

#define MFD_TYPE_SERVICE (mfd_service_get_type())
G_DECLARE_FINAL_TYPE(MfdService, mfd_service, MFD, SERVICE, GObject)

MfdService *mfd_service_new(sd_event *event,
                            GError  **error);

gboolean    mfd_service_listen(MfdService *service,
                               const char *socket_path,
                               GError    **error);

G_END_DECLS
