/*
 * Copyright (C) 2024-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mf-varlink-util.h"

#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <gio/gio.h>
#include <systemd/sd-daemon.h>

#include "mf-sdautoptr-private.h"

/**
 * mf_set_error_from_errno:
 */
void
mf_set_error_from_errno(GError **error, int negative_errno, const char *message)
{
    g_set_error(error,
                G_IO_ERROR,
                g_io_error_from_errno(-negative_errno),
                "%s: %s",
                message,
                g_strerror(-negative_errno));
}

/**
 * mf_varlink_get_peer_uid:
 */
int
mf_varlink_get_peer_uid(sd_varlink *link, uid_t *ret_uid)
{
    int r = sd_varlink_get_peer_uid(link, ret_uid);
    if (r < 0)
        g_warning("Unable to determine the user calling us: %s", g_strerror(-r));

    return r;
}

/**
 * mf_peer_is_privileged:
 *
 * Privileged callers are root, and the user we are running as. The latter
 * only makes a difference if we are not run by root ourselves, which is
 * useful for development.
 */
gboolean
mf_peer_is_privileged(uid_t peer_uid)
{
    return peer_uid == 0 || peer_uid == getuid();
}

/**
 * mf_varlink_server_new:
 * @event: The event loop to run the server in.
 * @name: Name of the service, for its description and info.
 * @flags: Flags in addition to the ones every service needs.
 * @interface: The interface the service implements.
 * @userdata: Userdata for the methods.
 * @error: a #GError
 *
 * Create a Varlink server. The caller binds the methods of @interface, and
 * the server does not listen for connections yet.
 */
sd_varlink_server *
mf_varlink_server_new(sd_event *event,
                      const char *name,
                      sd_varlink_server_flags_t flags,
                      const sd_varlink_interface *interface,
                      void *userdata,
                      GError **error)
{
    g_autoptr(sd_varlink_server) server = NULL;

    int r = sd_varlink_server_new(&server,
                                  SD_VARLINK_SERVER_ACCOUNT_UID | SD_VARLINK_SERVER_INHERIT_USERDATA | flags);
    if (r < 0) {
        mf_set_error_from_errno(error, r, "Unable to create Varlink server");
        return NULL;
    }

    sd_varlink_server_set_userdata(server, userdata);

    r = sd_varlink_server_set_description(server, name);
    if (r < 0) {
        mf_set_error_from_errno(error, r, "Unable to set description of the Varlink server");
        return NULL;
    }

    r = sd_varlink_server_set_info(server, "Manifold", name, MF_VERSION, "https://github.com/mfhq/manifold");
    if (r < 0) {
        mf_set_error_from_errno(error, r, "Unable to set Varlink service information");
        return NULL;
    }

    r = sd_varlink_server_add_interface(server, interface);
    if (r < 0) {
        mf_set_error_from_errno(error, r, "Unable to add Varlink interface");
        return NULL;
    }

    r = sd_varlink_server_attach_event(server, event, SD_EVENT_PRIORITY_NORMAL);
    if (r < 0) {
        mf_set_error_from_errno(error, r, "Unable to attach Varlink server to event loop");
        return NULL;
    }

    return g_steal_pointer(&server);
}

/**
 * mf_varlink_server_listen:
 * @server: The server.
 * @socket_path: (nullable): Path of the socket to listen on, or %NULL
 * @default_socket_path: Path of the socket to listen on if we were not socket activated.
 * @ret_activated: (out): Whether we listen on the socket we were activated with.
 * @error: A #GError
 *
 * Listen for connections. If no socket path is given, we listen on the socket
 * we were activated with, or on @default_socket_path if we were not started
 * via socket activation.
 */
gboolean
mf_varlink_server_listen(sd_varlink_server *server,
                         const char *socket_path,
                         const char *default_socket_path,
                         gboolean *ret_activated,
                         GError **error)
{
    g_autofree char *socket_dir = NULL;

    *ret_activated = FALSE;
    if (socket_path == NULL) {
        int r = sd_varlink_server_listen_auto(server);
        if (r < 0) {
            mf_set_error_from_errno(error, r, "Unable to listen on the passed socket");
            return FALSE;
        }
        if (r > 0) {
            g_info("Listening on the socket we were activated with");
            *ret_activated = TRUE;
            return TRUE;
        }

        socket_path = default_socket_path;
    }

    socket_dir = g_path_get_dirname(socket_path);
    if (g_mkdir_with_parents(socket_dir, 0755) != 0) {
        int saved_errno = errno;

        g_set_error(error,
                    G_IO_ERROR,
                    g_io_error_from_errno(saved_errno),
                    "Unable to create directory %s: %s",
                    socket_dir,
                    g_strerror(saved_errno));
        return FALSE;
    }

    /* every user may talk to us, and every method checks who calls it */
    int r = sd_varlink_server_listen_address(server, socket_path, 0666);
    if (r < 0) {
        g_set_error(error,
                    G_IO_ERROR,
                    g_io_error_from_errno(-r),
                    "Unable to listen on %s: %s",
                    socket_path,
                    g_strerror(-r));
        return FALSE;
    }

    g_info("Listening on %s", socket_path);
    return TRUE;
}

/**
 * mf_event_new_default:
 * @error: A #GError
 *
 * Get the default event loop, set up to quit cleanly on SIGINT and SIGTERM.
 */
sd_event *
mf_event_new_default(GError **error)
{
    sd_event *event = NULL;

    int r = sd_event_default(&event);
    if (r < 0) {
        mf_set_error_from_errno(error, r, "Unable to create event loop");
        return NULL;
    }

    r = sd_event_add_signal(event, NULL, SIGTERM | SD_EVENT_SIGNAL_PROCMASK, NULL, NULL);
    if (r >= 0)
        r = sd_event_add_signal(event, NULL, SIGINT | SD_EVENT_SIGNAL_PROCMASK, NULL, NULL);
    if (r < 0) {
        mf_set_error_from_errno(error, r, "Unable to handle signals");
        sd_event_unref(event);
        return NULL;
    }

    return event;
}

/**
 * mf_event_run:
 * @event: The event loop.
 *
 * Tell systemd we are ready, and run @event until we are asked to quit.
 *
 * Returns: The result of sd_event_loop()
 */
int
mf_event_run(sd_event *event)
{
    int r;

    sd_notify(0, "READY=1");
    r = sd_event_loop(event);
    sd_notify(0, "STOPPING=1");

    if (r < 0)
        g_critical("Event loop failed: %s", g_strerror(-r));
    return r;
}
