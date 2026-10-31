/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mfc-signing.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <glib/gi18n-lib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

#include "mfc-util.h"

/* the options of the key new command */
static char *optn_comment = NULL;
static gboolean optn_no_passphrase = FALSE;

static const GOptionEntry key_new_options[] = {
    {"comment",
     0, 0,
     G_OPTION_ARG_STRING, &optn_comment,
     N_("Store this comment with the key, instead of the name of the user and the host."),
     "TEXT"},
    {"no-passphrase",
     0, 0,
     G_OPTION_ARG_NONE,   &optn_no_passphrase,
     N_("Do not protect the key with a passphrase."),
     NULL  },
    G_OPTION_ENTRY_NULL,
};

/* the value of the --key option of the bundle sign command */
static char *optn_key = NULL;

static const GOptionEntry bundle_sign_options[] = {
    {"key",
     0, 0,
     G_OPTION_ARG_STRING, &optn_key,
     N_("Sign with this key: its name, the path of a key file, or the fingerprint of a key in ssh-agent."),
     "KEY"},
    G_OPTION_ENTRY_NULL,
};

/**
 * mfc_key_name_is_valid:
 *
 * Check that a key name can not be mistaken for a path, for a fingerprint
 * or for a public key.
 */
static gboolean
mfc_key_name_is_valid(const char *name)
{
    if (!g_ascii_isalnum(name[0]) || g_str_has_suffix(name, ".pub"))
        return FALSE;
    for (const char *c = name; *c != '\0'; c++) {
        if (!g_ascii_isalnum(*c) && strchr("._-", *c) == NULL)
            return FALSE;
    }

    return TRUE;
}

/**
 * mfc_generate_key:
 *
 * Create a key pair at @path. Unless the user chose not to have a passphrase,
 * ssh-keygen asks for one on the terminal.
 */
static gboolean
mfc_generate_key(const char *path, GError **error)
{
    g_autoptr(GStrvBuilder) builder = g_strv_builder_new();
    g_autoptr(GSubprocess) proc = NULL;
    g_auto(GStrv) args = NULL;

    g_strv_builder_add_many(builder, "ssh-keygen", "-q", "-t", "ed25519", "-f", path, NULL);
    if (optn_comment != NULL)
        g_strv_builder_add_many(builder, "-C", optn_comment, NULL);
    if (optn_no_passphrase)
        g_strv_builder_add_many(builder, "-N", "", NULL);
    args = g_strv_builder_end(builder);

    proc = g_subprocess_newv((const char *const *) args, G_SUBPROCESS_FLAGS_STDIN_INHERIT, error);
    if (proc == NULL)
        return FALSE;

    return g_subprocess_wait_check(proc, NULL, error);
}

/**
 * mfc_run_key_new_command:
 *
 * Create a signing key in the key directory of the user.
 */
static int
mfc_run_key_new_command(MfcContext *ctx, MfcCommand *cmd, int argc, char **argv)
{
    g_autoptr(GOptionContext) context = NULL;
    g_autoptr(MfSigner) signer = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree char *key_dir = NULL;
    g_autofree char *path = NULL;
    const char *name = MF_SIGNING_KEY_DEFAULT_NAME;
    int ret;

    context = mfc_option_context_for_command(cmd, key_new_options);
    ret = mfc_parse_command_options(context, cmd, &argc, &argv);
    if (ret != MFC_EXIT_SUCCESS)
        return ret;

    if (argc > 2) {
        mfc_print_help_hint(cmd->name, argv[2]);
        return MFC_EXIT_INVALID_CMD;
    }
    if (argc == 2)
        name = argv[1];
    if (!mfc_key_name_is_valid(name)) {
        g_printerr(_("'%s' is not a valid key name: it has to start with a letter or digit, contain only "
                     "those and '.', '_' or '-', and can not end in '.pub'.\n"),
                   name);
        return MFC_EXIT_INVALID_CMD;
    }

    /* without a terminal, ssh-keygen would silently create a key without a passphrase */
    if (!optn_no_passphrase && !isatty(STDIN_FILENO)) {
        g_printerr("%s\n",
                   _("There is no terminal to ask for a passphrase, use --no-passphrase to go without."));
        return MFC_EXIT_BAD_INPUT;
    }

    key_dir = mf_signer_build_key_dir();
    path = g_build_filename(key_dir, name, NULL);
    if (g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_printerr(_("A key named '%s' already exists.\n"), name);
        return MFC_EXIT_FAILURE;
    }
    if (g_mkdir_with_parents(key_dir, 0700) != 0) {
        g_printerr(_("Unable to create %s: %s\n"), key_dir, g_strerror(errno));
        return MFC_EXIT_FAILURE;
    }

    if (!mfc_generate_key(path, &error)) {
        g_printerr(_("Unable to create the key '%s': %s\n"), name, error->message);
        return MFC_EXIT_FAILURE;
    }
    signer = mf_signer_new_for_key(name, &error);
    if (signer == NULL) {
        g_printerr("%s\n", error->message);
        return MFC_EXIT_FAILURE;
    }

    g_print(_("Created the signing key '%s' in %s\n"), name, key_dir);
    g_print(_("Public key: %s\n"), mf_signer_get_public_key(signer));
    g_print(_("Fingerprint: %s\n"), mf_signer_get_fingerprint(signer));

    return MFC_EXIT_SUCCESS;
}

/**
 * mfc_key_get_comment:
 *
 * Returns: the comment of the public key, which may be empty
 */
static const char *
mfc_key_get_comment(MfSigner *key)
{
    const char *public_key = mf_signer_get_public_key(key);
    const char *comment;

    /* "<type> <base64 key> [comment]" */
    if (public_key == NULL || (comment = strchr(public_key, ' ')) == NULL ||
        (comment = strchr(comment + 1, ' ')) == NULL)
        return "";

    return comment + 1;
}

/**
 * mfc_run_key_list_command:
 *
 * List the signing keys in the key directory of the user.
 */
static int
mfc_run_key_list_command(MfcContext *ctx, MfcCommand *cmd, int argc, char **argv)
{
    g_autoptr(GOptionContext) context = NULL;
    g_autoptr(GPtrArray) keys = NULL;
    g_autoptr(GString) table = g_string_new(NULL);
    g_autoptr(GError) error = NULL;
    /* TRANSLATORS: column headers of the table of signing keys */
    const char *name_header = _("NAME");
    const char *fingerprint_header = _("FINGERPRINT");
    const char *comment_header = _("COMMENT");
    gsize name_width;
    gsize fingerprint_width;
    int ret;

    context = mfc_option_context_for_command(cmd, NULL);
    ret = mfc_parse_command_options(context, cmd, &argc, &argv);
    if (ret != MFC_EXIT_SUCCESS)
        return ret;

    if (argc > 1) {
        mfc_print_help_hint(cmd->name, argv[1]);
        return MFC_EXIT_INVALID_CMD;
    }

    keys = mf_signer_list_keys(&error);
    if (keys == NULL) {
        g_printerr(_("Unable to list the signing keys: %s\n"), error->message);
        return MFC_EXIT_FAILURE;
    }
    if (keys->len == 0) {
        g_print("%s\n", _("There are no signing keys. Run 'manifold key new' to create one."));
        return MFC_EXIT_SUCCESS;
    }

    /* the columns are as wide as what they contain */
    name_width = g_utf8_strlen(name_header, -1);
    fingerprint_width = g_utf8_strlen(fingerprint_header, -1);
    for (guint i = 0; i < keys->len; i++) {
        MfSigner *key = g_ptr_array_index(keys, i);
        const char *fingerprint = mf_signer_get_fingerprint(key);

        name_width = MAX(name_width, strlen(mf_signer_get_name(key)));
        if (fingerprint != NULL)
            fingerprint_width = MAX(fingerprint_width, strlen(fingerprint));
    }

    mfc_string_append_column(table, name_header, name_width);
    mfc_string_append_column(table, fingerprint_header, fingerprint_width);
    g_string_append_printf(table, "%s\n", comment_header);
    for (guint i = 0; i < keys->len; i++) {
        MfSigner *key = g_ptr_array_index(keys, i);
        const char *fingerprint = mf_signer_get_fingerprint(key);

        mfc_string_append_column(table, mf_signer_get_name(key), name_width);
        mfc_string_append_column(table, fingerprint != NULL ? fingerprint : "-", fingerprint_width);
        g_string_append_printf(table, "%s\n", mfc_key_get_comment(key));
    }
    g_print("%s", table->str);

    return MFC_EXIT_SUCCESS;
}

/**
 * mfc_run_bundle_sign_command:
 *
 * Sign bundle files, replacing any signature they have.
 */
static int
mfc_run_bundle_sign_command(MfcContext *ctx, MfcCommand *cmd, int argc, char **argv)
{
    g_autoptr(GOptionContext) context = NULL;
    g_autoptr(MfSigner) signer = NULL;
    g_autoptr(GError) error = NULL;
    int ret;

    context = mfc_option_context_for_command(cmd, bundle_sign_options);
    ret = mfc_parse_command_options(context, cmd, &argc, &argv);
    if (ret != MFC_EXIT_SUCCESS)
        return ret;

    if (argc < 2) {
        g_printerr("%s\n", _("You need to specify a bundle file to sign."));
        return MFC_EXIT_BAD_INPUT;
    }

    if (optn_key != NULL)
        signer = mf_signer_new_for_key(optn_key, &error);
    else
        signer = mf_signer_new_default(&error);
    if (signer == NULL) {
        g_printerr("%s\n", error->message);
        if (optn_key == NULL)
            g_printerr("%s\n", _("Run 'manifold key new' to create a signing key."));
        return MFC_EXIT_BAD_INPUT;
    }

    for (int i = 1; i < argc; i++) {
        g_autoptr(MfBundleWriter) writer = mf_bundle_writer_new_for_file(argv[i], &error);
        g_autoptr(GString) skipped = NULL;

        if (writer != NULL && mf_bundle_writer_finish(writer, signer, &error)) {
            /* TRANSLATORS: the bundle file, and the name of the signing key */
            g_print(_("Signed %s with %s\n"), argv[i], mf_signer_get_name(signer));
            continue;
        }
        g_printerr("%s\n", error->message);

        /* we stop at the first file that fails, and say which ones we did not get to */
        if (i + 1 < argc) {
            skipped = g_string_new(argv[i + 1]);
            for (int j = i + 2; j < argc; j++)
                g_string_append_printf(skipped, ", %s", argv[j]);
            g_printerr(_("The remaining files were not signed: %s\n"), skipped->str);
        }
        return MFC_EXIT_FAILURE;
    }

    return MFC_EXIT_SUCCESS;
}

/**
 * mfc_register_signing_commands:
 */
void
mfc_register_signing_commands(MfcContext *ctx)
{
    mfc_context_register_command(ctx,
                                 "key new",
                                 "[NAME]",
                                 /* TRANSLATORS: `manifold key new` command description */
                                 _("Create a signing key."),
                                 mfc_run_key_new_command);
    mfc_context_register_command(ctx,
                                 "key list",
                                 NULL,
                                 /* TRANSLATORS: `manifold key list` command description */
                                 _("List your signing keys."),
                                 mfc_run_key_list_command);
    mfc_context_register_command(ctx,
                                 "bundle sign",
                                 "FILES",
                                 /* TRANSLATORS: `manifold bundle sign` command description */
                                 _("Sign bundle files, replacing any signature they have."),
                                 mfc_run_bundle_sign_command);
}
