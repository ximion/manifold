/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

#include "mfc-context.h"

G_BEGIN_DECLS

void            mfc_print_help_hint(const char *command,
                                    const char *unexpected_argument);

GOptionContext *mfc_option_context_for_command(MfcCommand         *cmd,
                                               const GOptionEntry *entries);
int             mfc_parse_command_options(GOptionContext *context,
                                          MfcCommand     *cmd,
                                          int            *argc,
                                          char         ***argv);

void            mfc_string_append_column(GString    *row,
                                         const char *text,
                                         gsize       width);
char           *mfc_format_components(GPtrArray *components,
                                      gboolean   with_header);
void            mfc_print_components(GPtrArray *components,
                                     gboolean   with_header);
guint           mfc_prompt_number(const char *question,
                                  guint       maxnum);

G_END_DECLS
