/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mfc-util.h"

#include <stdio.h>
#include <string.h>

#define BIN_NAME "manifold"

void
mfc_print_help_hint(const char *command, const char *unknown_option)
{
    if (unknown_option != NULL)
        g_printerr("Option '%s' is unknown.\n", unknown_option);

    if (command == NULL)
        g_printerr("Run '%s --help' to see a full list of available command line options.\n", BIN_NAME);
    else
        g_printerr(
            "Run '%s --help' to see a list of available commands and options, "
            "and '%s %s --help' to see a list of options specific for this command.\n",
            BIN_NAME,
            BIN_NAME,
            command);
}

/**
 * mfc_option_context_for_command:
 *
 * Create the option context for a command.
 */
GOptionContext *
mfc_option_context_for_command(MfcCommand *cmd, const GOptionEntry *entries)
{
    GOptionContext *context;
    g_autofree char *parameter_str = NULL;
    g_autofree char *summary = NULL;

    if (cmd->arguments == NULL)
        parameter_str = g_strdup(cmd->name);
    else
        parameter_str = g_strconcat(cmd->name, " ", cmd->arguments, NULL);

    context = g_option_context_new(parameter_str);
    g_option_context_set_help_enabled(context, TRUE);
    if (entries != NULL)
        g_option_context_add_main_entries(context, entries, NULL);

    summary = g_strdup_printf("Manifold command-line interface\n '%s' command\n %s", cmd->name, cmd->summary);
    g_option_context_set_summary(context, summary);

    return context;
}

/**
 * mfc_parse_command_options:
 *
 * Parse the options of a command, printing errors.
 *
 * Returns: %MFC_EXIT_SUCCESS if the options were parsed, and error code otherwise
 */
int
mfc_parse_command_options(GOptionContext *context, MfcCommand *cmd, int *argc, char ***argv)
{
    g_autoptr(GError) error = NULL;

    if (!g_option_context_parse(context, argc, argv, &error)) {
        g_printerr("%s\n", error->message);
        mfc_print_help_hint(cmd == NULL ? NULL : cmd->name, NULL);
        return MFC_EXIT_INVALID_CMD;
    }

    return MFC_EXIT_SUCCESS;
}

/**
 * mfc_format_components:
 *
 * Format a table of components, one per line.
 *
 * Returns: (transfer full): the table
 */
char *
mfc_format_components(GPtrArray *components, gboolean with_header)
{
    g_autoptr(GString) table = g_string_new(NULL);
    gsize id_width = with_header ? strlen("COMPONENT") : 0;
    gsize version_width = with_header ? strlen("VERSION") : 0;

    /* the columns are as wide as what they contain */
    for (guint i = 0; i < components->len; i++) {
        MfComponent *component = g_ptr_array_index(components, i);

        id_width = MAX(id_width, strlen(mf_component_get_id(component)));
        version_width = MAX(version_width, strlen(mf_component_get_full_version(component)));
    }

    if (with_header)
        g_string_append_printf(table,
                               "%-8s %-*s %-*s %s\n",
                               "KIND",
                               (int) id_width,
                               "COMPONENT",
                               (int) version_width,
                               "VERSION",
                               "FLAGS");

    for (guint i = 0; i < components->len; i++) {
        MfComponent *component = g_ptr_array_index(components, i);
        g_autoptr(GString) flags = g_string_new(NULL);

        if (mf_component_is_sdk(component))
            g_string_append(flags, "sdk ");
        if (mf_component_is_current(component))
            g_string_append(flags, "current ");
        if (mf_component_get_install_mode(component) == MF_INSTALL_MODE_MANUAL)
            g_string_append(flags, "manual ");
        if (flags->len > 0)
            g_string_truncate(flags, flags->len - 1);

        g_string_append_printf(table,
                               "%-8s %-*s %-*s %s\n",
                               mf_component_kind_to_string(mf_component_get_kind(component)),
                               (int) id_width,
                               mf_component_get_id(component),
                               (int) version_width,
                               mf_component_get_full_version(component),
                               flags->str);
    }

    return g_string_free(g_steal_pointer(&table), FALSE);
}

/**
 * mfc_print_components:
 *
 * Print a table of components, one per line.
 */
void
mfc_print_components(GPtrArray *components, gboolean with_header)
{
    g_autofree char *table = mfc_format_components(components, with_header);

    g_print("%s", table);
}

/**
 * mfc_prompt_number:
 *
 * Ask the user for a number from 0 to @maxnum.
 *
 * Returns: the number, or 0 if the input ended
 */
guint
mfc_prompt_number(const char *question, guint maxnum)
{
    char buffer[64];
    unsigned int answer;

    g_print("%s ", question);
    for (;;) {
        if (fgets(buffer, sizeof(buffer), stdin) == NULL)
            return 0;
        if (sscanf(buffer, "%u", &answer) == 1 && answer <= maxnum)
            return answer;
        g_print("Please enter a number from 0 to %u: ", maxnum);
    }
}
