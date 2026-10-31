/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <locale.h>
#include <glib.h>
#include <glib/gi18n-lib.h>
#include <manifold.h>

#include "mfc-context.h"
#include "mfc-manage.h"
#include "mfc-query.h"
#include "mfc-signing.h"
#include "mfc-util.h"

/* global options, valid for all commands */
static gboolean optn_show_version = FALSE;
static gboolean optn_verbose = FALSE;
static char *optn_socket_path = NULL;
static gboolean optn_no_ask_password = FALSE;

static const GOptionEntry mfc_global_options[] = {
    {"version",         0, 0, G_OPTION_ARG_NONE, &optn_show_version,    N_("Show the program version."),         NULL},
    {"verbose",         0, 0, G_OPTION_ARG_NONE, &optn_verbose,         N_("Show extra debugging information."), NULL},
    {"socket",
     0,                    0,
     G_OPTION_ARG_FILENAME,                      &optn_socket_path,
     N_("Reach the Manifold service at this socket."),
     "PATH"                                                                                                          },
    {"no-ask-password",
     0,                    0,
     G_OPTION_ARG_NONE,                          &optn_no_ask_password,
     N_("Do not ask for a password to authorize an operation."),
     NULL                                                                                                            },
    G_OPTION_ENTRY_NULL,
};

static char *
mfc_get_help_summary(MfcContext *ctx)
{
    g_autoptr(GString) summary = g_string_new(NULL);

    /* TRANSLATORS: header of the list of commands in the --help output */
    g_string_append_printf(summary, "%s\n\n%s\n", _("Manifold command-line interface"), _("Commands:"));

    for (guint i = 0; i < ctx->commands->len; i++) {
        const MfcCommand *cmd = g_ptr_array_index(ctx->commands, i);
        g_autofree char *usage = mfc_command_build_usage(cmd);

        g_string_append_printf(summary, "  %-32s %s\n", usage, cmd->summary);
    }
    g_string_truncate(summary, summary->len - 1);

    return g_string_free(g_steal_pointer(&summary), FALSE);
}

/**
 * mfc_global_option_takes_value:
 *
 * Whether @arg is a global option whose value is the next argument.
 */
static gboolean
mfc_global_option_takes_value(const char *arg)
{
    for (const GOptionEntry *entry = mfc_global_options; entry->long_name != NULL; entry++) {
        if (entry->arg == G_OPTION_ARG_NONE)
            continue;
        if ((g_str_has_prefix(arg, "--") && g_str_equal(arg + 2, entry->long_name)) ||
            (entry->short_name != 0 && arg[0] == '-' && arg[1] == entry->short_name && arg[2] == '\0'))
            return TRUE;
    }

    return FALSE;
}

/**
 * mfc_wants_subcommand_help:
 *
 * Check if we need to display the help text of a subcommand.
 */
static gboolean
mfc_wants_subcommand_help(int argc, char **argv)
{
    gboolean have_command = FALSE;

    for (int i = 1; i < argc; i++) {
        if (!g_str_has_prefix(argv[i], "-")) {
            have_command = TRUE;
            continue;
        }
        /* the value of a global option is not the command */
        if (!have_command && mfc_global_option_takes_value(argv[i])) {
            i++;
            continue;
        }
        if (have_command && (g_strcmp0(argv[i], "--help") == 0 || g_strcmp0(argv[i], "-h") == 0))
            return TRUE;
    }

    return FALSE;
}

/**
 * mfc_find_command_for_args:
 *
 * Find the command that the arguments select. The name of a grouped command,
 * like "key new", spans two arguments, of which we drop the first one.
 *
 * Returns: (transfer none): the command, or %NULL
 */
static MfcCommand *
mfc_find_command_for_args(MfcContext *ctx, int *argc, char ***argv)
{
    if (*argc > 2) {
        g_autofree char *name = g_strconcat((*argv)[1], " ", (*argv)[2], NULL);
        MfcCommand *cmd = mfc_context_find_command(ctx, name);

        if (cmd != NULL) {
            (*argc)--;
            (*argv)++;
            return cmd;
        }
    }

    return mfc_context_find_command(ctx, (*argv)[1]);
}

int
main(int argc, char **argv)
{
    g_autoptr(GOptionContext) context = NULL;
    g_autoptr(MfcContext) ctx = NULL;
    MfcCommand *cmd;
    gboolean show_global_help;
    int ret;

    /* bind locale */
    setlocale(LC_ALL, "");
    bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
    bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
    textdomain(GETTEXT_PACKAGE);

    ctx = mfc_context_new();
    mfc_register_query_commands(ctx);
    mfc_register_manage_commands(ctx);
    mfc_register_signing_commands(ctx);

    context = g_option_context_new("COMMAND");
    g_option_context_add_main_entries(context, mfc_global_options, NULL);

    /* unknown options are handled by the subcommands */
    g_option_context_set_ignore_unknown_options(context, TRUE);

    /* a command handles its own --help, we only display the global help here */
    show_global_help = !mfc_wants_subcommand_help(argc, argv);
    g_option_context_set_help_enabled(context, show_global_help);
    if (show_global_help) {
        g_autofree char *summary = mfc_get_help_summary(ctx);
        g_option_context_set_summary(context, summary);
    }

    ret = mfc_parse_command_options(context, NULL, &argc, &argv);
    if (ret != MFC_EXIT_SUCCESS)
        return ret;

    if (optn_show_version) {
        g_print(_("Manifold version: %s\n"), mf_version_string());
        return MFC_EXIT_SUCCESS;
    }

    if (argc < 2) {
        g_printerr("%s\n", _("You need to specify a command."));
        mfc_print_help_hint(NULL, NULL);
        return MFC_EXIT_INVALID_CMD;
    }

    ctx->verbose = optn_verbose;
    ctx->socket_path = g_strdup(optn_socket_path);
    ctx->ask_password = !optn_no_ask_password;
    if (ctx->verbose)
        g_log_set_debug_enabled(TRUE);

    cmd = mfc_find_command_for_args(ctx, &argc, &argv);
    if (cmd == NULL) {
        if (mfc_context_is_command_group(ctx, argv[1]))
            g_printerr(_("Command '%s' needs a subcommand.\n"), argv[1]);
        else
            g_printerr(_("Command '%s' is unknown.\n"), argv[1]);
        mfc_print_help_hint(NULL, NULL);
        return MFC_EXIT_INVALID_CMD;
    }

    /* run the command function */
    return cmd->func(ctx, cmd, argc - 1, argv + 1);
}
