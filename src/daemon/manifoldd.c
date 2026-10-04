/*
 * Copyright (C) 2024-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <glib.h>
#include <systemd/sd-event.h>

#include "mf-sdautoptr-private.h"
#include "mf-varlink-util.h"
#include "mfd-service.h"

static int
mfd_run_service(const char *socket_path)
{
    g_autoptr(sd_event) event = NULL;
    g_autoptr(MfdService) service = NULL;
    g_autoptr(GError) error = NULL;

    event = mf_event_new_default(&error);
    if (event == NULL) {
        g_critical("%s", error->message);
        return EXIT_FAILURE;
    }

    service = mfd_service_new(event, &error);
    if (service == NULL) {
        g_critical("%s", error->message);
        return EXIT_FAILURE;
    }

    if (!mfd_service_listen(service, socket_path, &error)) {
        g_critical("%s", error->message);
        return EXIT_FAILURE;
    }

    return mf_event_run(event) < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

int
main(int argc, char **argv)
{
    g_autoptr(GOptionContext) context = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree char *socket_path = NULL;
    gboolean verbose = FALSE;
    gboolean show_version = FALSE;
    const GOptionEntry entries[] = {
        {"socket",  0, 0, G_OPTION_ARG_FILENAME, &socket_path,  "Listen on the socket at PATH", "PATH"},
        {"verbose", 0, 0, G_OPTION_ARG_NONE,     &verbose,      "Show more information",        NULL  },
        {"version", 0, 0, G_OPTION_ARG_NONE,     &show_version, "Show the program version",     NULL  },
        G_OPTION_ENTRY_NULL,
    };

    context = g_option_context_new("- Manifold management daemon");
    g_option_context_set_description(context, "The default socket is " MF_SOCKET_PATH);
    g_option_context_add_main_entries(context, entries, NULL);
    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("%s\n", error->message);
        return EXIT_FAILURE;
    }

    if (argc > 1) {
        g_printerr("Unexpected argument: %s\n", argv[1]);
        return EXIT_FAILURE;
    }

    if (show_version) {
        g_print("manifoldd %s\n", MF_VERSION);
        return EXIT_SUCCESS;
    }

    if (verbose)
        g_log_set_debug_enabled(TRUE);

    /* make what we create world-readable, regardless of the inherited umask */
    umask(022);

    return mfd_run_service(socket_path);
}
