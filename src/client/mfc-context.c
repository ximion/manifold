/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mfc-context.h"

#include <unistd.h>

#include "mf-macros-private.h"

MfcContext *
mfc_context_new(void)
{
    MfcContext *ctx = g_new0(MfcContext, 1);

    ctx->commands = g_ptr_array_new_with_free_func(g_free);
    ctx->is_tty = isatty(STDOUT_FILENO);
    ctx->progress_bar = mfc_progress_bar_new(ctx->is_tty);

    return ctx;
}

void
mfc_context_free(MfcContext *ctx)
{
    if (ctx == NULL)
        return;

    g_clear_object(&ctx->manager);
    mfc_progress_bar_free(ctx->progress_bar);
    g_ptr_array_unref(ctx->commands);
    g_free(ctx->socket_path);
    g_free(ctx);
}

/**
 * mfc_context_register_command:
 *
 * Register a command.
 */
void
mfc_context_register_command(MfcContext *ctx,
                             const char *name,
                             const char *arguments,
                             const char *summary,
                             MfcCommandFunc func)
{
    MfcCommand *cmd = g_new0(MfcCommand, 1);
    cmd->name = name;
    cmd->arguments = arguments;
    cmd->summary = summary;
    cmd->func = func;

    g_ptr_array_add(ctx->commands, cmd);
}

/**
 * mfc_context_find_command:
 *
 * Returns: (transfer none): the command with this name, or %NULL
 */
MfcCommand *
mfc_context_find_command(MfcContext *ctx, const char *name)
{
    for (guint i = 0; i < ctx->commands->len; i++) {
        MfcCommand *cmd = g_ptr_array_index(ctx->commands, i);

        if (g_strcmp0(cmd->name, name) == 0)
            return cmd;
    }

    return NULL;
}

/**
 * mfc_command_build_usage:
 *
 * Returns: (transfer full): the name of the command, followed by its arguments
 */
char *
mfc_command_build_usage(const MfcCommand *cmd)
{
    if (cmd->arguments == NULL)
        return g_strdup(cmd->name);
    return g_strconcat(cmd->name, " ", cmd->arguments, NULL);
}

/**
 * mfc_context_get_manager:
 *
 * Returns: (transfer none): the manager of the service, created on first use
 */
MfManager *
mfc_context_get_manager(MfcContext *ctx)
{
    if (ctx->manager == NULL) {
        ctx->manager = mf_manager_new();
        if (ctx->socket_path != NULL)
            mf_manager_set_socket_path(ctx->manager, ctx->socket_path);
        if (!ctx->ask_password) {
            MfTransactionFlags flags = mf_manager_get_transaction_flags(ctx->manager);

            mf_flags_remove(flags, MF_TRANSACTION_FLAG_INTERACTIVE);
            mf_manager_set_transaction_flags(ctx->manager, flags);
        }
    }

    return ctx->manager;
}
