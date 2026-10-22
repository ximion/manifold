/*
 * Copyright (C) 2024-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mfd-policy.h"

#include <errno.h>

#include <manifold.h>
#include "mf-varlink-util.h"

/* polkit may have to prompt the user, so give it plenty of time */
constexpr uint64_t POLKIT_CALL_TIMEOUT_USEC = 5 * 60 * 1000 * 1000ULL;

/* CheckAuthorizationFlags: AllowUserInteraction */
constexpr uint32_t POLKIT_ALLOW_USER_INTERACTION = 1;

/**
 * mfd_policy_connect:
 * @event: The event loop that processes the replies of polkit.
 * @error: A #GError
 *
 * Connect to the system bus, so we can ask polkit for authorization.
 *
 * Returns: (transfer full): The bus connection, or %NULL on error.
 */
sd_bus *
mfd_policy_connect(sd_event *event, GError **error)
{
    sd_bus *bus = NULL;

    int r = sd_bus_open_system(&bus);
    if (r < 0) {
        mf_set_error_from_errno(error, r, "Unable to connect to the system bus");
        return NULL;
    }

    r = sd_bus_attach_event(bus, event, SD_EVENT_PRIORITY_NORMAL);
    if (r < 0) {
        mf_set_error_from_errno(error, r, "Unable to attach the system bus to the event loop");
        sd_bus_flush_close_unref(bus);
        return NULL;
    }

    return bus;
}

/**
 * mfd_policy_check_async:
 * @bus: The system bus, see mfd_policy_connect().
 * @link: The connection of the caller.
 * @uid: The calling user.
 * @action_id: The polkit action the caller needs.
 * @interactive: %TRUE if polkit may ask the user to authenticate.
 * @callback: Called with the reply of polkit, which mfd_policy_read_reply() reads.
 * @userdata: Passed to @callback.
 * @ret_slot: (out): The pending call. Unreferencing it cancels the call.
 * @error: A #GError
 *
 * Ask polkit whether the caller on @link is authorized for an action.
 * This does not wait for the answer, as polkit may ask the user for a
 * password, which can take minutes.
 *
 * Returns: %TRUE if the question was sent to polkit.
 */
gboolean
mfd_policy_check_async(sd_bus *bus,
                       sd_varlink *link,
                       uid_t uid,
                       const char *action_id,
                       gboolean interactive,
                       sd_bus_message_handler_t callback,
                       void *userdata,
                       sd_bus_slot **ret_slot,
                       GError **error)
{
    sd_bus_message *request = NULL;
    int pidfd;
    int r;

    pidfd = sd_varlink_get_peer_pidfd(link);
    if (pidfd < 0) {
        mf_set_error_from_errno(error, pidfd, "Unable to identify the process calling us");
        return FALSE;
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
        sd_bus_message_unref(request);
        return FALSE;
    }

    r = sd_bus_call_async(bus, ret_slot, request, callback, userdata, POLKIT_CALL_TIMEOUT_USEC);
    sd_bus_message_unref(request);
    if (r < 0) {
        mf_set_error_from_errno(error, r, "Unable to ask polkit for authorization");
        return FALSE;
    }

    return TRUE;
}

/**
 * mfd_policy_read_reply:
 * @reply: The reply of polkit to mfd_policy_check_async().
 * @error: A #GError
 *
 * Read whether polkit authorized the caller.
 *
 * Returns: 1 if the caller is authorized, 0 if it is not, a negative errno on error.
 */
int
mfd_policy_read_reply(sd_bus_message *reply, GError **error)
{
    const sd_bus_error *bus_error = sd_bus_message_get_error(reply);
    int authorized = 0;
    int challenge = 0;
    int r;

    if (bus_error != NULL) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_FAILED,
                    "Unable to ask polkit for authorization: %s",
                    bus_error->message != NULL ? bus_error->message : bus_error->name);
        return -EIO;
    }

    r = sd_bus_message_enter_container(reply, 'r', "bba{ss}");
    if (r >= 0)
        r = sd_bus_message_read(reply, "bb", &authorized, &challenge);
    if (r < 0) {
        mf_set_error_from_errno(error, r, "Reply of polkit was not understood");
        return r;
    }

    return authorized ? 1 : 0;
}
