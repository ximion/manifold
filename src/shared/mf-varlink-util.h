/*
 * Copyright (C) 2024-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <sys/types.h>
#include <glib.h>
#include <systemd/sd-event.h>
#include <systemd/sd-varlink.h>

#include "mf-macros-private.h"

MF_BEGIN_PRIVATE_DECLS

void               mf_set_error_from_errno(GError    **error,
                                           int         negative_errno,
                                           const char *message);

int                mf_varlink_get_peer_uid(sd_varlink *link,
                                           uid_t      *ret_uid);
gboolean           mf_peer_is_privileged(uid_t peer_uid);

sd_varlink_server *mf_varlink_server_new(sd_event                   *event,
                                         const char                 *name,
                                         sd_varlink_server_flags_t   flags,
                                         const sd_varlink_interface *interface,
                                         void                       *userdata,
                                         GError                    **error);
gboolean           mf_varlink_server_listen(sd_varlink_server *server,
                                            const char        *socket_path,
                                            const char        *default_socket_path,
                                            gboolean          *ret_activated,
                                            GError           **error);

sd_event          *mf_event_new_default(GError **error);
int                mf_event_run(sd_event *event);

MF_END_PRIVATE_DECLS
