/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mfc-util.h"

#include <stdio.h>
#include <string.h>
#include <glib/gi18n-lib.h>

#define BIN_NAME "manifold"

void
mfc_print_help_hint(const char *command, const char *unexpected_argument)
{
    if (unexpected_argument != NULL)
        g_printerr(_("Argument '%s' is not expected.\n"), unexpected_argument);

    if (command == NULL)
        /* TRANSLATORS: %s is the name of the program, 'manifold' */
        g_printerr(_("Run '%s --help' to see a full list of available command line options.\n"), BIN_NAME);
    else
        g_printerr(
            /* TRANSLATORS: %s is the name of the program, 'manifold' */
            _("Run '%s --help' to see a list of available commands and options, "
              "and '%s %s --help' to see a list of options specific for this command.\n"),
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
    g_autofree char *title = NULL;
    g_autofree char *summary = NULL;

    parameter_str = mfc_command_build_usage(cmd);
    context = g_option_context_new(parameter_str);
    g_option_context_set_help_enabled(context, TRUE);
    if (entries != NULL)
        g_option_context_add_main_entries(context, entries, NULL);

    /* TRANSLATORS: header of the --help output of a command, %s is the name of the command */
    title = g_strdup_printf(_("'%s' command"), cmd->name);
    summary = g_strdup_printf("%s\n %s\n %s", _("Manifold command-line interface"), title, cmd->summary);
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
 * mfc_string_append_column:
 *
 * Append a column of a table row: @text, padded with spaces to @width
 * characters, and the space that separates it from the next column.
 */
static void
mfc_string_append_column(GString *row, const char *text, gsize width)
{
    g_string_append(row, text);
    for (gsize len = g_utf8_strlen(text, -1); len < width; len++)
        g_string_append_c(row, ' ');
    g_string_append_c(row, ' ');
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
    /* TRANSLATORS: column headers of the table of components */
    const char *kind_header = _("KIND");
    const char *id_header = _("COMPONENT");
    const char *version_header = _("VERSION");
    const char *flags_header = _("FLAGS");
    /* the kind column fits the longest kind, "platform" */
    gsize kind_width = 8;
    gsize id_width = 0;
    gsize version_width = 0;

    /* the columns are as wide as what they contain */
    if (with_header) {
        kind_width = MAX(kind_width, (gsize) g_utf8_strlen(kind_header, -1));
        id_width = g_utf8_strlen(id_header, -1);
        version_width = g_utf8_strlen(version_header, -1);
    }
    for (guint i = 0; i < components->len; i++) {
        MfComponent *component = g_ptr_array_index(components, i);

        id_width = MAX(id_width, strlen(mf_component_get_id(component)));
        version_width = MAX(version_width, strlen(mf_component_get_full_version(component)));
    }

    if (with_header) {
        mfc_string_append_column(table, kind_header, kind_width);
        mfc_string_append_column(table, id_header, id_width);
        mfc_string_append_column(table, version_header, version_width);
        g_string_append_printf(table, "%s\n", flags_header);
    }

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
                               "%-*s %-*s %-*s %s\n",
                               (int) kind_width,
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
        g_print(_("Please enter a number from 0 to %u: "), maxnum);
    }
}
