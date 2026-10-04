/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mfc-query.h"

#include "mfc-util.h"

/* the value of the --kind option of the list command */
static char *optn_kind = NULL;

static const GOptionEntry list_options[] = {
    {"kind",
     0, 0,
     G_OPTION_ARG_STRING, &optn_kind,
     "Only list components of this kind: core, platform, kit, app or plugin.", "KIND"},
    G_OPTION_ENTRY_NULL,
};

/**
 * mfc_run_list_command:
 *
 * List what is installed.
 */
static int
mfc_run_list_command(MfcContext *ctx, MfcCommand *cmd, int argc, char **argv)
{
    g_autoptr(GOptionContext) context = NULL;
    g_autoptr(MfStore) store = NULL;
    g_autoptr(GPtrArray) components = NULL;
    g_autoptr(GError) error = NULL;
    MfComponentKind kind = MF_COMPONENT_KIND_UNKNOWN;
    const char *component_id = NULL;
    int ret;

    context = mfc_option_context_for_command(cmd, list_options);
    ret = mfc_parse_command_options(context, cmd, &argc, &argv);
    if (ret != MFC_EXIT_SUCCESS)
        return ret;

    if (argc > 2) {
        mfc_print_help_hint(cmd->name, argv[2]);
        return MFC_EXIT_INVALID_CMD;
    }
    if (argc == 2) {
        component_id = argv[1];
        if (!mf_component_id_is_valid(component_id)) {
            g_printerr("'%s' is not a valid component identifier.\n", component_id);
            return MFC_EXIT_INVALID_CMD;
        }
    }
    if (optn_kind != NULL) {
        kind = mf_component_kind_from_string(optn_kind);
        if (kind == MF_COMPONENT_KIND_UNKNOWN) {
            g_printerr("Kind '%s' is unknown. Use core, platform, kit, app or plugin.\n", optn_kind);
            return MFC_EXIT_INVALID_CMD;
        }
    }

    store = mf_store_new();
    components = mf_store_list_components(store, component_id, kind, &error);
    if (components == NULL) {
        g_printerr("Unable to list components: %s\n", error->message);
        return MFC_EXIT_FAILURE;
    }

    if (components->len == 0) {
        g_print("Nothing is installed.\n");
        return MFC_EXIT_SUCCESS;
    }
    mfc_print_components(components, TRUE);

    return MFC_EXIT_SUCCESS;
}

/**
 * mfc_register_query_commands:
 */
void
mfc_register_query_commands(MfcContext *ctx)
{
    mfc_context_register_command(ctx,
                                 "list",
                                 "[COMPONENT-ID]",
                                 "List the installed components, or the versions of one.",
                                 mfc_run_list_command);
}
