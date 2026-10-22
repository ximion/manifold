/*
 * Copyright (C) 2024-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>
#include <systemd/sd-varlink.h>

G_BEGIN_DECLS

/* the actions, as declared in org.freedesktop.manifold.policy */
#define MFD_POLKIT_ACTION_INSTALL           "org.freedesktop.manifold.install"
#define MFD_POLKIT_ACTION_INSTALL_UNTRUSTED "org.freedesktop.manifold.install-untrusted"
#define MFD_POLKIT_ACTION_REMOVE            "org.freedesktop.manifold.remove"
#define MFD_POLKIT_ACTION_MARK              "org.freedesktop.manifold.mark"

sd_bus  *mfd_policy_connect(sd_event *event,
                            GError  **error);

gboolean mfd_policy_check_async(sd_bus                  *bus,
                                sd_varlink              *link,
                                uid_t                    uid,
                                const char              *action_id,
                                gboolean                 interactive,
                                sd_bus_message_handler_t callback,
                                void                    *userdata,
                                sd_bus_slot            **ret_slot,
                                GError                 **error);
int      mfd_policy_read_reply(sd_bus_message *reply,
                               GError        **error);

G_END_DECLS
