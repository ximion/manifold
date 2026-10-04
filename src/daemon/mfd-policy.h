/*
 * Copyright (C) 2024-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>
#include <systemd/sd-varlink.h>
#include <manifold.h>

G_BEGIN_DECLS

/* the actions, as declared in org.freedesktop.manifold.policy */
#define MFD_POLKIT_ACTION_INSTALL           "org.freedesktop.manifold.install"
#define MFD_POLKIT_ACTION_INSTALL_UNTRUSTED "org.freedesktop.manifold.install-untrusted"
#define MFD_POLKIT_ACTION_REMOVE            "org.freedesktop.manifold.remove"
#define MFD_POLKIT_ACTION_MARK              "org.freedesktop.manifold.mark"

int mfd_policy_check_authorized(sd_varlink        *link,
                                const char        *method,
                                const char        *action_id,
                                MfTransactionFlags flags,
                                uid_t             *ret_uid);

G_END_DECLS
