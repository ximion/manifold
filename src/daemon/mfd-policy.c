/*
 * Copyright (C) 2024-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mfd-policy.h"

#include <unistd.h>
#include <systemd/sd-bus.h>

#include <manifold.h>
#include "mf-interface-private.h"
#include "mf-varlink-util.h"

/* polkit may have to prompt the user, so give it plenty of time */
constexpr uint64_t POLKIT_CALL_TIMEOUT_USEC = 5 * 60 * 1000 * 1000ULL;

/* CheckAuthorizationFlags: AllowUserInteraction */
constexpr uint32_t POLKIT_ALLOW_USER_INTERACTION = 1;

/**
 * mfd_polkit_check_authorization:
 * @link: the connection the method was called on
 * @uid: the calling user
 * @action_id: the polkit action
 * @interactive: whether to allow user interaction
 * @error: a #GError
 *
 * Ask polkit whether the peer of a connection is authorized for an action.
 * This blocks until polkit has an answer.
 *
 * Returns: 1 if the peer is authorized, 0 if it is not, a negative on error
 */
static int
mfd_polkit_check_authorization(sd_varlink *link,
                               uid_t uid,
                               const char *action_id,
                               gboolean interactive,
                               GError **error)
{
    sd_bus *bus = NULL;
    sd_bus_message *request = NULL;
    sd_bus_message *reply = NULL;
    sd_bus_error bus_error = SD_BUS_ERROR_NULL;
    int pidfd;
    int authorized = 0;
    int challenge = 0;

    pidfd = sd_varlink_get_peer_pidfd(link);
    if (pidfd < 0) {
        mf_set_error_from_errno(error, pidfd, "Unable to identify the process calling us");
        return pidfd;
    }

    int r = sd_bus_default_system(&bus);
    if (r < 0) {
        mf_set_error_from_errno(error, r, "Unable to connect to the system bus");
        goto out;
    }

    r = sd_bus_message_new_method_call(bus,
                                       &request,
                                       "org.freedesktop.PolicyKit1",
                                       "/org/freedesktop/PolicyKit1/Authority",
                                       "org.freedesktop.PolicyKit1.Authority",
                                       "CheckAuthorization");
    if (r >= 0)
        r = sd_bus_message_append(request,
                                  "(sa{sv})",
                                  "unix-process",
                                  2,
                                  "pidfd",
                                  "h",
                                  pidfd,
                                  "uid",
                                  "i",
                                  (int32_t) uid);
    if (r >= 0)
        r = sd_bus_message_append(request,
                                  "sa{ss}us",
                                  action_id,
                                  0,
                                  interactive ? POLKIT_ALLOW_USER_INTERACTION : 0,
                                  "");
    if (r < 0) {
        mf_set_error_from_errno(error, r, "Unable to build request for polkit");
        goto out;
    }

    r = sd_bus_call(bus, request, POLKIT_CALL_TIMEOUT_USEC, &bus_error, &reply);
    if (r < 0) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_FAILED,
                    "Unable to ask polkit for authorization: %s",
                    bus_error.message != NULL ? bus_error.message : g_strerror(-r));
        goto out;
    }

    r = sd_bus_message_enter_container(reply, 'r', "bba{ss}");
    if (r >= 0)
        r = sd_bus_message_read(reply, "bb", &authorized, &challenge);
    if (r < 0) {
        mf_set_error_from_errno(error, r, "Reply of polkit was not understood");
        goto out;
    }

    r = authorized ? 1 : 0;

out:
    sd_bus_error_free(&bus_error);
    sd_bus_message_unref(reply);
    sd_bus_message_unref(request);
    sd_bus_flush_close_unref(bus);

    return r;
}

/**
 * mfd_policy_check_authorized:
 * @link: the connection the method was called on
 * @method: the method that was called, for the log
 * @action_id: (nullable): the polkit action the method needs, or %NULL if every user may call it
 * @flags: the transaction flags of the call
 * @ret_uid: (out): the calling user
 *
 * Root, and the user we are running as, may call everything.
 * Everyone else needs PolicyKit authorization for what the method does to the host.
 * On error and authorization failure, the call is replied to.
 *
 * Returns: 0 if the caller may proceed, an error code otherwise.
 */
int
mfd_policy_check_authorized(sd_varlink *link,
                            const char *method,
                            const char *action_id,
                            MfTransactionFlags flags,
                            uid_t *ret_uid)
{
    g_autoptr(GError) error = NULL;
    uid_t peer_uid;

    int r = mf_varlink_get_peer_uid(link, &peer_uid);
    if (r < 0)
        return r;
    *ret_uid = peer_uid;

    if (action_id == NULL || mf_peer_is_privileged(peer_uid))
        return 0;

    r = mfd_polkit_check_authorization(link,
                                       peer_uid,
                                       action_id,
                                       mf_flags_contains(flags, MF_TRANSACTION_FLAG_INTERACTIVE),
                                       &error);
    if (r < 0) {
        g_warning("Unable to authorize user %u for %s: %s", (unsigned int) peer_uid, method, error->message);
        return sd_varlink_errorbo(link,
                                  MF_VARLINK_ERROR_FAILED,
                                  SD_JSON_BUILD_PAIR_STRING("message", error->message));
    }
    if (r == 0) {
        g_info("Denied call of %s by user %u: not authorized for %s",
               method,
               (unsigned int) peer_uid,
               action_id);
        return sd_varlink_error(link, SD_VARLINK_ERROR_PERMISSION_DENIED, NULL);
    }

    g_info("User %u is authorized for %s", (unsigned int) peer_uid, action_id);
    return 0;
}
