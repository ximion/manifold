/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>
#include <manifold.h>

#include "mfc-progress-bar.h"

G_BEGIN_DECLS

#define MFC_EXIT_SUCCESS     0
#define MFC_EXIT_FAILURE     1
#define MFC_EXIT_INVALID_CMD 2
#define MFC_EXIT_BAD_INPUT   3
#define MFC_EXIT_NO_RESULT   4

typedef struct MfcCommand MfcCommand;

/**
 * MfcContext:
 */
typedef struct {
    MfManager      *manager;
    MfcProgressBar *progress_bar;
    GPtrArray      *commands; /* of MfcCommand */

    /* global options */
    char           *socket_path;
    gboolean        verbose;
    gboolean        ask_password;

    gboolean        is_tty;
} MfcContext;

/**
 * MfcCommandFunc:
 * @ctx: the context
 * @cmd: the command that is being run
 * @argc: argument count, with the last word of the command name as first argument
 * @argv: argument vector, with the last word of the command name as first argument
 *
 * Returns: the exit code
 */
typedef int (*MfcCommandFunc)(MfcContext *ctx,
                              MfcCommand *cmd,
                              int         argc,
                              char      **argv);

/**
 * MfcCommand:
 *
 * A command of the tool. The name of a grouped command has two words,
 * like "key new".
 */
struct MfcCommand {
    const char    *name;
    const char    *arguments;
    const char    *summary;
    MfcCommandFunc func;
};

MfcContext *mfc_context_new(void);
void        mfc_context_free(MfcContext *ctx);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(MfcContext,
                              mfc_context_free)

void        mfc_context_register_command(MfcContext    *ctx,
                                         const char    *name,
                                         const char    *arguments,
                                         const char    *summary,
                                         MfcCommandFunc func);
MfcCommand *mfc_context_find_command(MfcContext *ctx,
                                     const char *name);
gboolean    mfc_context_is_command_group(MfcContext *ctx,
                                         const char *name);
char       *mfc_command_build_usage(const MfcCommand *cmd);

MfManager  *mfc_context_get_manager(MfcContext *ctx);

G_END_DECLS
