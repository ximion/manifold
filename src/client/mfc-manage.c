/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mfc-manage.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "mfc-util.h"

/**
 * mfc_on_progress:
 *
 * Draw the progress of an operation, one bar per item.
 */
static void
mfc_on_progress(const MfProgress *progress, gpointer user_data)
{
    MfcContext *ctx = user_data;
    const MfItemProgress *item = mf_progress_get_item(progress);
    g_autofree char *label = NULL;

    if (item == NULL)
        return;
    const char *subject = mf_item_progress_get_subject(item);
    const char *version = mf_item_progress_get_version(item);

    switch (mf_item_progress_get_action(item)) {
    case MF_ACTION_INSTALL:
        label = g_strdup_printf("Installing %s %s", subject, version ? version : "");
        break;
    case MF_ACTION_REMOVE:
        label = g_strdup_printf("Removing %s %s", subject, version ? version : "");
        break;
    case MF_ACTION_CLEANUP:
        label = g_strdup_printf("Cleaning up %.12s", subject);
        break;
    default:
        label = g_strdup(subject);
        break;
    }

    if (g_strcmp0(mfc_progress_bar_get_label(ctx->progress_bar), label) != 0)
        mfc_progress_bar_start(ctx->progress_bar, label);
    mfc_progress_bar_update(ctx->progress_bar,
                            mf_item_progress_get_percent(item),
                            mf_item_progress_get_bytes_done(item));
}

/**
 * mfc_print_result:
 *
 * Print the results of an operation.
 */
static void
mfc_print_result(MfResult *result)
{
    GPtrArray *installed = mf_result_get_installed(result);
    GPtrArray *removed = mf_result_get_removed(result);
    guint n_images = mf_result_get_removed_images(result)->len;

    if (installed->len > 0) {
        g_print("Installed:\n");
        mfc_print_components(installed, FALSE);
    }
    if (removed->len > 0) {
        g_print("Removed:\n");
        mfc_print_components(removed, FALSE);
    }
    if (n_images > 0)
        g_print("Removed %u image%s nothing uses anymore.\n", n_images, n_images == 1 ? "" : "s");
    if (mf_result_get_bytes_added(result) > 0) {
        g_autofree char *size = g_format_size(mf_result_get_bytes_added(result));

        g_print("Disk space used: %s\n", size);
    }
    if (mf_result_get_bytes_freed(result) > 0) {
        g_autofree char *size = g_format_size(mf_result_get_bytes_freed(result));

        g_print("Disk space freed: %s\n", size);
    }
}

/**
 * mfc_install_bundle:
 *
 * Install the components of one bundle file.
 */
static int
mfc_install_bundle(MfcContext *ctx, const char *fname)
{
    g_autoptr(MfResult) result = NULL;
    g_autofree char *basename = NULL;
    g_autoptr(GError) error = NULL;

    int fd = open(fname, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        g_printerr("Unable to open %s: %s\n", fname, g_strerror(errno));
        return MFC_EXIT_BAD_INPUT;
    }

    basename = g_path_get_basename(fname);
    result = mf_manager_install_file(mfc_context_get_manager(ctx), fd, mfc_on_progress, ctx, NULL, &error);
    mfc_progress_bar_finish(ctx->progress_bar);
    close(fd);
    if (result == NULL) {
        g_printerr("Unable to install %s: %s\n", basename, error->message);
        return MFC_EXIT_FAILURE;
    }

    mfc_print_result(result);

    return MFC_EXIT_SUCCESS;
}

/**
 * mfc_run_install_command:
 *
 * Install the components of bundle files.
 */
static int
mfc_run_install_command(MfcContext *ctx, MfcCommand *cmd, int argc, char **argv)
{
    g_autoptr(GOptionContext) context = NULL;
    int ret;

    context = mfc_option_context_for_command(cmd, NULL);
    ret = mfc_parse_command_options(context, cmd, &argc, &argv);
    if (ret != MFC_EXIT_SUCCESS)
        return ret;

    if (argc < 2) {
        g_printerr("You need to specify a bundle file to install.\n");
        return MFC_EXIT_BAD_INPUT;
    }

    for (int i = 1; i < argc; i++) {
        ret = mfc_install_bundle(ctx, argv[i]);
        if (ret != MFC_EXIT_SUCCESS)
            return ret;
    }

    return MFC_EXIT_SUCCESS;
}

/**
 * mfc_choose_version:
 *
 * If several versions of a component are installed, let the user choose
 * which one to remove.
 *
 * Returns: the exit code. With %MFC_EXIT_SUCCESS, @ret_version holds the
 *   chosen version, or %NULL if all versions were chosen
 */
static int
mfc_choose_version(MfcContext *ctx, const char *component_id, char **ret_version)
{
    g_autoptr(MfStore) store = mf_store_new();
    g_autoptr(GPtrArray) versions = NULL;
    g_autoptr(GError) error = NULL;
    guint choice;

    *ret_version = NULL;

    if (!mf_component_id_is_valid(component_id)) {
        g_printerr("'%s' is not a valid component identifier.\n", component_id);
        return MFC_EXIT_INVALID_CMD;
    }
    versions = mf_store_list_components(store, component_id, MF_COMPONENT_KIND_UNKNOWN, &error);
    if (versions == NULL) {
        g_printerr("Unable to look up %s: %s\n", component_id, error->message);
        return MFC_EXIT_FAILURE;
    }
    if (versions->len <= 1)
        return MFC_EXIT_SUCCESS;

    if (!isatty(STDIN_FILENO)) {
        g_printerr("Several versions of %s are installed, specify the one to remove:\n", component_id);
        mfc_print_components(versions, FALSE);
        return MFC_EXIT_FAILURE;
    }

    g_print("Several versions of %s are installed:\n", component_id);
    for (guint i = 0; i < versions->len; i++) {
        MfComponent *component = g_ptr_array_index(versions, i);

        g_print("  [%u] %s%s\n",
                i + 1,
                mf_component_get_version(component),
                mf_component_is_current(component) ? " (current)" : "");
    }
    g_print("  [%u] all of them\n", versions->len + 1);

    choice = mfc_prompt_number("Which one should be removed? [0 to abort]", versions->len + 1);
    if (choice == 0) {
        g_print("Aborted.\n");
        return MFC_EXIT_FAILURE;
    }
    if (choice <= versions->len) {
        MfComponent *component = g_ptr_array_index(versions, choice - 1);

        *ret_version = g_strdup(mf_component_get_version(component));
    }

    return MFC_EXIT_SUCCESS;
}

/**
 * mfc_run_remove_command:
 *
 * Remove a component, or one version of it.
 */
static int
mfc_run_remove_command(MfcContext *ctx, MfcCommand *cmd, int argc, char **argv)
{
    g_autoptr(GOptionContext) context = NULL;
    g_autoptr(MfResult) result = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree char *chosen_version = NULL;
    const char *component_id;
    const char *version = NULL;
    int ret;

    context = mfc_option_context_for_command(cmd, NULL);
    ret = mfc_parse_command_options(context, cmd, &argc, &argv);
    if (ret != MFC_EXIT_SUCCESS)
        return ret;

    if (argc < 2) {
        g_printerr("You need to specify the component to remove.\n");
        return MFC_EXIT_INVALID_CMD;
    }
    if (argc > 3) {
        mfc_print_help_hint(cmd->name, argv[3]);
        return MFC_EXIT_INVALID_CMD;
    }
    component_id = argv[1];
    if (argc > 2) {
        version = argv[2];
    } else {
        ret = mfc_choose_version(ctx, component_id, &chosen_version);
        if (ret != MFC_EXIT_SUCCESS)
            return ret;
        version = chosen_version;
    }

    result = mf_manager_remove_component(mfc_context_get_manager(ctx),
                                         component_id,
                                         version,
                                         mfc_on_progress,
                                         ctx,
                                         NULL,
                                         &error);
    mfc_progress_bar_finish(ctx->progress_bar);
    if (result == NULL) {
        g_printerr("Unable to remove %s: %s\n", component_id, error->message);
        return MFC_EXIT_FAILURE;
    }

    mfc_print_result(result);

    return MFC_EXIT_SUCCESS;
}

/**
 * mfc_run_cleanup_command:
 *
 * Remove what nothing needs anymore: dependencies that nothing builds on,
 * and images that no component uses.
 */
static int
mfc_run_cleanup_command(MfcContext *ctx, MfcCommand *cmd, int argc, char **argv)
{
    g_autoptr(GOptionContext) context = NULL;
    g_autoptr(MfResult) result = NULL;
    g_autoptr(GError) error = NULL;
    int ret;

    context = mfc_option_context_for_command(cmd, NULL);
    ret = mfc_parse_command_options(context, cmd, &argc, &argv);
    if (ret != MFC_EXIT_SUCCESS)
        return ret;

    if (argc > 1) {
        mfc_print_help_hint(cmd->name, argv[1]);
        return MFC_EXIT_INVALID_CMD;
    }

    result = mf_manager_cleanup(mfc_context_get_manager(ctx), mfc_on_progress, ctx, NULL, &error);
    mfc_progress_bar_finish(ctx->progress_bar);
    if (result == NULL) {
        g_printerr("Unable to clean up: %s\n", error->message);
        return MFC_EXIT_FAILURE;
    }

    if (mf_result_get_removed(result)->len == 0 && mf_result_get_removed_images(result)->len == 0) {
        g_print("Nothing to clean up.\n");
        return MFC_EXIT_SUCCESS;
    }
    mfc_print_result(result);

    return MFC_EXIT_SUCCESS;
}

/**
 * mfc_register_manage_commands:
 */
void
mfc_register_manage_commands(MfcContext *ctx)
{
    mfc_context_register_command(ctx,
                                 "install",
                                 "FILES",
                                 "Install the components of bundle files.",
                                 mfc_run_install_command);
    mfc_context_register_command(ctx,
                                 "remove",
                                 "COMPONENT-ID [VERSION]",
                                 "Remove a component, or one version of it.",
                                 mfc_run_remove_command);
    mfc_context_register_command(ctx,
                                 "cleanup",
                                 NULL,
                                 "Remove what nothing needs anymore: unused dependencies and images.",
                                 mfc_run_cleanup_command);
}
