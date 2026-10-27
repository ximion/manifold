/*
 * Copyright (C) 2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mf-signer.h"

#include <string.h>
#include <glib/gi18n-lib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

#include "mf-error.h"

/**
 * MfSigner:
 *
 * A signer to sign Manifold artifacts with an SSH key.
 *
 * The signatures are SSHSig signatures made by `ssh-keygen -Y sign`.
 * The key is a private key file, the text of a private key, or a key held by ssh-agent.
 *
 * The signing keys of a user are kept in `$XDG_CONFIG_HOME/manifold/keys/`,
 * one private key file per name, with the public key next to it.
 */

typedef struct {
    char *name;
    char *key_file;    /* a private key, or a public key whose private half is in ssh-agent */
    char *key_data;    /* the text of a private key */
    char *public_key;  /* the public key in OpenSSH format, if we know it */
    char *fingerprint; /* the SHA256 fingerprint of the public key, if we know it */
} MfSignerPrivate;

G_DEFINE_TYPE_WITH_PRIVATE(MfSigner, mf_signer, G_TYPE_OBJECT)

#define GET_PRIVATE(o) ((MfSignerPrivate *) mf_signer_get_instance_private(o))

#define PUBLIC_KEY_SUFFIX  ".pub"
#define FINGERPRINT_PREFIX "SHA256:"

/**
 * mf_signer_finalize:
 */
static void
mf_signer_finalize(GObject *object)
{
    MfSignerPrivate *priv = GET_PRIVATE(MF_SIGNER(object));

    /* do not leave the private key behind in freed memory */
    if (priv->key_data != NULL)
        explicit_bzero(priv->key_data, strlen(priv->key_data));

    g_free(priv->name);
    g_free(priv->key_file);
    g_free(priv->key_data);
    g_free(priv->public_key);
    g_free(priv->fingerprint);

    G_OBJECT_CLASS(mf_signer_parent_class)->finalize(object);
}

/**
 * mf_signer_class_init:
 */
static void
mf_signer_class_init(MfSignerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = mf_signer_finalize;
}

/**
 * mf_signer_init:
 */
static void
mf_signer_init(MfSigner *signer)
{
}

/**
 * mf_ssh_key_fingerprint:
 * @public_key: a public key in OpenSSH format, `<type> <base64 key> [comment]`
 *
 * Compute the fingerprint of a public key the way `ssh-keygen -l` shows it:
 * the SHA256 digest of the key, in base64 without padding.
 *
 * Returns: (transfer full) (nullable): the fingerprint, or %NULL if @public_key is not a public key
 */
static char *
mf_ssh_key_fingerprint(const char *public_key)
{
    g_auto(GStrv) parts = g_strsplit(public_key, " ", 3);
    g_autoptr(GChecksum) checksum = NULL;
    g_autofree guchar *blob = NULL;
    g_autofree char *encoded = NULL;
    guint8 digest[32];
    gsize digest_len = sizeof(digest);
    gsize blob_len = 0;
    gsize len;

    if (g_strv_length(parts) < 2)
        return NULL;
    blob = g_base64_decode(parts[1], &blob_len);
    if (blob_len == 0)
        return NULL;

    checksum = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(checksum, blob, blob_len);
    g_checksum_get_digest(checksum, digest, &digest_len);

    encoded = g_base64_encode(digest, digest_len);
    len = strlen(encoded);
    while (len > 0 && encoded[len - 1] == '=')
        encoded[--len] = '\0';

    return g_strconcat(FINGERPRINT_PREFIX, encoded, NULL);
}

/**
 * mf_signer_set_public_key:
 *
 * Set the public key of the signer, and its fingerprint along with it.
 */
static void
mf_signer_set_public_key(MfSigner *signer, const char *public_key)
{
    MfSignerPrivate *priv = GET_PRIVATE(signer);

    g_free(priv->public_key);
    g_free(priv->fingerprint);
    priv->public_key = g_strstrip(g_strdup(public_key));
    priv->fingerprint = mf_ssh_key_fingerprint(priv->public_key);
}

/**
 * mf_signer_new_for_key_file:
 * @path: a private key file, or a public key file whose private half is in ssh-agent
 *
 * Create a signer for a key file. If a public key file `<path>.pub` is next
 * to the private key, we read the public key from it.
 *
 * Returns: (transfer full): a new #MfSigner
 */
MfSigner *
mf_signer_new_for_key_file(const char *path)
{
    MfSigner *signer;
    MfSignerPrivate *priv;
    g_autofree char *public_key_file = NULL;
    g_autofree char *public_key = NULL;

    g_return_val_if_fail(path != NULL, NULL);

    signer = g_object_new(MF_TYPE_SIGNER, NULL);
    priv = GET_PRIVATE(signer);
    priv->name = g_strdup(path);
    priv->key_file = g_strdup(path);

    if (g_str_has_suffix(path, PUBLIC_KEY_SUFFIX))
        public_key_file = g_strdup(path);
    else
        public_key_file = g_strconcat(path, PUBLIC_KEY_SUFFIX, NULL);
    if (g_file_get_contents(public_key_file, &public_key, NULL, NULL))
        mf_signer_set_public_key(signer, public_key);

    return signer;
}

/**
 * mf_signer_new_for_key_data:
 * @private_key: the text of a private key, as it would be stored in a key file
 *
 * Create a signer for a private key that is not in a file, for example one
 * that was handed to us in an environment variable.
 * We do not know the public key of such a signer.
 *
 * Returns: (transfer full): a new #MfSigner
 */
MfSigner *
mf_signer_new_for_key_data(const char *private_key)
{
    MfSigner *signer;
    MfSignerPrivate *priv;

    g_return_val_if_fail(private_key != NULL, NULL);

    signer = g_object_new(MF_TYPE_SIGNER, NULL);
    priv = GET_PRIVATE(signer);
    /* ssh-keygen refuses a key file that does not end with a newline, which an
     * environment variable often lacks */
    if (g_str_has_suffix(private_key, "\n"))
        priv->key_data = g_strdup(private_key);
    else
        priv->key_data = g_strconcat(private_key, "\n", NULL);

    return signer;
}

/**
 * mf_signer_new_for_agent:
 * @public_key: the public key in OpenSSH format, as `ssh-add -L` shows it
 *
 * Create a signer for a key held by ssh-agent. The agent is found
 * through `$SSH_AUTH_SOCK`.
 *
 * Returns: (transfer full): a new #MfSigner
 */
MfSigner *
mf_signer_new_for_agent(const char *public_key)
{
    MfSigner *signer;
    MfSignerPrivate *priv;

    g_return_val_if_fail(public_key != NULL, NULL);

    signer = g_object_new(MF_TYPE_SIGNER, NULL);
    priv = GET_PRIVATE(signer);
    mf_signer_set_public_key(signer, public_key);
    priv->name = g_strdup(priv->fingerprint != NULL ? priv->fingerprint : priv->public_key);

    return signer;
}

/**
 * mf_signer_build_key_dir:
 *
 * Build the path of the directory that holds the signing keys of the user.
 *
 * Returns: (transfer full): the key directory
 */
char *
mf_signer_build_key_dir(void)
{
    return g_build_filename(g_get_user_config_dir(), "manifold", "keys", NULL);
}

/**
 * mf_signer_new_for_key_name:
 *
 * Create a signer for a key in the key directory of the user.
 *
 * Returns: (transfer full): a new #MfSigner, or %NULL if there is no key of that name
 */
static MfSigner *
mf_signer_new_for_key_name(const char *name, GError **error)
{
    g_autofree char *key_dir = mf_signer_build_key_dir();
    g_autofree char *path = g_build_filename(key_dir, name, NULL);
    MfSigner *signer;

    if (!g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
        g_set_error(error, MF_ERROR, MF_ERROR_NOT_FOUND, _("There is no signing key named “%s”"), name);
        return NULL;
    }

    signer = mf_signer_new_for_key_file(path);
    g_free(GET_PRIVATE(signer)->name);
    GET_PRIVATE(signer)->name = g_strdup(name);

    return signer;
}

/**
 * mf_signer_list_agent_keys:
 *
 * Find the keys that ssh-agent holds. Without a running agent, there are none.
 *
 * Returns: (transfer full) (element-type MfSigner): the keys in ssh-agent
 */
static GPtrArray *
mf_signer_list_agent_keys(void)
{
    GPtrArray *keys = g_ptr_array_new_with_free_func(g_object_unref);
    g_autofree char *ssh_add = NULL;
    g_autoptr(GSubprocess) proc = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree char *output = NULL;
    g_auto(GStrv) lines = NULL;

    if (g_getenv("SSH_AUTH_SOCK") == NULL)
        return keys;
    ssh_add = g_find_program_in_path("ssh-add");
    if (ssh_add == NULL)
        return keys;

    proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                            &error,
                            ssh_add,
                            "-L",
                            NULL);
    if (proc == NULL || !g_subprocess_communicate_utf8(proc, NULL, NULL, &output, NULL, &error)) {
        g_debug("Unable to list the keys of ssh-agent: %s", error->message);
        return keys;
    }
    /* ssh-add fails if the agent has no keys, or if it can not be reached */
    if (!g_subprocess_get_successful(proc))
        return keys;

    lines = g_strsplit(output, "\n", -1);
    for (guint i = 0; lines[i] != NULL; i++) {
        if (lines[i][0] != '\0')
            g_ptr_array_add(keys, mf_signer_new_for_agent(lines[i]));
    }

    return keys;
}

/**
 * mf_signer_new_for_key:
 * @key: the key to sign with
 *
 * Create a signer for a key the user selected. @key is the name of a key in
 * the key directory of the user, the path of a key file if it contains a `/`,
 * or the fingerprint of a key in ssh-agent if it starts with `SHA256:`.
 *
 * Returns: (transfer full): a new #MfSigner, or %NULL if there is no such key
 */
MfSigner *
mf_signer_new_for_key(const char *key, GError **error)
{
    g_return_val_if_fail(key != NULL, NULL);

    if (g_str_has_prefix(key, FINGERPRINT_PREFIX)) {
        g_autoptr(GPtrArray) agent_keys = mf_signer_list_agent_keys();

        for (guint i = 0; i < agent_keys->len; i++) {
            MfSigner *signer = g_ptr_array_index(agent_keys, i);

            if (g_strcmp0(GET_PRIVATE(signer)->fingerprint, key) == 0)
                return g_object_ref(signer);
        }
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_NOT_FOUND,
                    _("ssh-agent holds no key with the fingerprint %s"),
                    key);
        return NULL;
    }

    if (strchr(key, '/') != NULL) {
        if (!g_file_test(key, G_FILE_TEST_IS_REGULAR)) {
            g_set_error(error, MF_ERROR, MF_ERROR_NOT_FOUND, _("The signing key %s does not exist"), key);
            return NULL;
        }
        return mf_signer_new_for_key_file(key);
    }

    return mf_signer_new_for_key_name(key, error);
}

/**
 * mf_signer_new_default:
 *
 * Create a signer for the key we sign with if the user selected none:
 * the private key in `$MANIFOLD_SIGNING_KEY` if it is set, or else the
 * key named `default` in the key directory of the user.
 *
 * Returns: (transfer full): a new #MfSigner, or %NULL with %MF_ERROR_NOT_FOUND if neither exists
 */
MfSigner *
mf_signer_new_default(GError **error)
{
    const char *key_data = g_getenv(MF_SIGNING_KEY_ENV);
    MfSigner *signer;

    if (key_data != NULL && key_data[0] != '\0') {
        signer = mf_signer_new_for_key_data(key_data);
        GET_PRIVATE(signer)->name = g_strdup("$" MF_SIGNING_KEY_ENV);
        return signer;
    }

    signer = mf_signer_new_for_key_name(MF_SIGNING_KEY_DEFAULT_NAME, NULL);
    if (signer == NULL) {
        g_set_error(
            error,
            MF_ERROR,
            MF_ERROR_NOT_FOUND,
            /* TRANSLATORS: the first placeholder is an environment variable, the second the name of a key */
            _("No signing key was selected: $%s is not set, and there is no key named \"%s\""),
            MF_SIGNING_KEY_ENV,
            MF_SIGNING_KEY_DEFAULT_NAME);
        return NULL;
    }

    return signer;
}

/**
 * mf_signer_compare_names:
 */
static int
mf_signer_compare_names(gconstpointer a, gconstpointer b)
{
    MfSigner *signer_a = *((MfSigner **) a);
    MfSigner *signer_b = *((MfSigner **) b);

    return g_strcmp0(GET_PRIVATE(signer_a)->name, GET_PRIVATE(signer_b)->name);
}

/**
 * mf_signer_list_keys:
 *
 * Find the keys the user can sign with: the keys in the key directory of
 * the user, sorted by name, followed by the keys that ssh-agent holds.
 *
 * Returns: (transfer full) (element-type MfSigner): the keys, or %NULL on error
 */
GPtrArray *
mf_signer_list_keys(GError **error)
{
    g_autoptr(GPtrArray) keys = g_ptr_array_new_with_free_func(g_object_unref);
    g_autoptr(GPtrArray) agent_keys = NULL;
    g_autofree char *key_dir = mf_signer_build_key_dir();
    g_autoptr(GDir) dir = NULL;
    g_autoptr(GError) local_error = NULL;
    const char *name;

    dir = g_dir_open(key_dir, 0, &local_error);
    if (dir == NULL && !g_error_matches(local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
        g_propagate_error(error, g_steal_pointer(&local_error));
        return NULL;
    }

    while (dir != NULL && (name = g_dir_read_name(dir)) != NULL) {
        g_autoptr(MfSigner) signer = NULL;

        /* public keys only accompany the private keys */
        if (name[0] == '.' || g_str_has_suffix(name, PUBLIC_KEY_SUFFIX))
            continue;
        signer = mf_signer_new_for_key_name(name, NULL);
        if (signer != NULL)
            g_ptr_array_add(keys, g_steal_pointer(&signer));
    }
    g_ptr_array_sort(keys, mf_signer_compare_names);

    agent_keys = mf_signer_list_agent_keys();
    g_ptr_array_extend_and_steal(keys, g_steal_pointer(&agent_keys));

    return g_steal_pointer(&keys);
}

/**
 * mf_signer_get_name:
 * @signer: an #MfSigner
 *
 * Get the name the key was selected by: its name in the key directory of the
 * user, the path of its key file, its fingerprint if it is in ssh-agent, or
 * `$MANIFOLD_SIGNING_KEY` if it was read from there.
 *
 * Returns: (nullable): the name, or %NULL for a key that was handed to us directly
 */
const char *
mf_signer_get_name(MfSigner *signer)
{
    g_return_val_if_fail(MF_IS_SIGNER(signer), NULL);
    return GET_PRIVATE(signer)->name;
}

/**
 * mf_signer_get_public_key:
 * @signer: an #MfSigner
 *
 * Returns: (nullable): the public key in OpenSSH format, or %NULL if we do not know it
 */
const char *
mf_signer_get_public_key(MfSigner *signer)
{
    g_return_val_if_fail(MF_IS_SIGNER(signer), NULL);
    return GET_PRIVATE(signer)->public_key;
}

/**
 * mf_signer_get_fingerprint:
 * @signer: an #MfSigner
 *
 * Returns: (nullable): the SHA256 fingerprint of the key, or %NULL if we do not know its public key
 */
const char *
mf_signer_get_fingerprint(MfSigner *signer)
{
    g_return_val_if_fail(MF_IS_SIGNER(signer), NULL);
    return GET_PRIVATE(signer)->fingerprint;
}

/**
 * mf_find_ssh_keygen:
 *
 * Returns: (transfer full): the path of ssh-keygen, or %NULL if it is not installed
 */
static char *
mf_find_ssh_keygen(GError **error)
{
    char *ssh_keygen = g_find_program_in_path("ssh-keygen");

    if (ssh_keygen == NULL)
        g_set_error_literal(error,
                            MF_ERROR,
                            MF_ERROR_NOT_FOUND,
                            _("Signing needs ssh-keygen, which is not installed"));
    return ssh_keygen;
}

/**
 * mf_signer_check:
 * @signer: an #MfSigner
 *
 * Check that we can sign with @signer, before there is anything to sign.
 * This does not load the key, so it can still turn out to be unusable.
 *
 * Returns: %TRUE if we can try to sign
 */
gboolean
mf_signer_check(MfSigner *signer, GError **error)
{
    MfSignerPrivate *priv = GET_PRIVATE(signer);
    g_autofree char *ssh_keygen = NULL;

    g_return_val_if_fail(MF_IS_SIGNER(signer), FALSE);

    ssh_keygen = mf_find_ssh_keygen(error);
    if (ssh_keygen == NULL)
        return FALSE;
    if (priv->key_file != NULL && !g_file_test(priv->key_file, G_FILE_TEST_IS_REGULAR)) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_NOT_FOUND,
                    _("The signing key %s does not exist"),
                    priv->key_file);
        return FALSE;
    }

    return TRUE;
}

/**
 * mf_signer_run_ssh_keygen:
 *
 * Sign @data with the key in @key_file.
 *
 * Returns: (transfer full): the signature, or %NULL on error
 */
static GBytes *
mf_signer_run_ssh_keygen(MfSigner *signer,
                         const char *key_file,
                         const char *sig_namespace,
                         GBytes *data,
                         GError **error)
{
    MfSignerPrivate *priv = GET_PRIVATE(signer);
    g_autofree char *ssh_keygen = NULL;
    g_autoptr(GSubprocess) proc = NULL;
    g_autoptr(GBytes) signature = NULL;
    g_autoptr(GBytes) stderr_data = NULL;
    g_autofree char *message = NULL;

    ssh_keygen = mf_find_ssh_keygen(error);
    if (ssh_keygen == NULL)
        return NULL;

    /* ssh-keygen signs what it reads on stdin and writes the signature to stdout */
    proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                G_SUBPROCESS_FLAGS_STDERR_PIPE,
                            error,
                            ssh_keygen,
                            "-Y",
                            "sign",
                            "-f",
                            key_file,
                            "-n",
                            sig_namespace,
                            NULL);
    if (proc == NULL)
        return NULL;
    if (!g_subprocess_communicate(proc, data, NULL, &signature, &stderr_data, error))
        return NULL;

    if (!g_subprocess_get_successful(proc)) {
        message = g_strndup(g_bytes_get_data(stderr_data, NULL), g_bytes_get_size(stderr_data));
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_FAILED,
                    _("Unable to sign with %s: %s"),
                    priv->name != NULL ? priv->name : _("the given key"),
                    g_strstrip(message));
        return NULL;
    }

    return g_steal_pointer(&signature);
}

/**
 * mf_signer_sign:
 * @signer: an #MfSigner
 * @sig_namespace: the SSHSig namespace to sign in, like %MF_SIGNATURE_NAMESPACE_BUNDLE
 * @data: the data to sign
 *
 * Sign @data with the key of @signer. If the key is protected by a
 * passphrase, ssh-keygen asks for it on the terminal.
 *
 * Returns: (transfer full): the armored SSHSig signature, or %NULL on error
 */
GBytes *
mf_signer_sign(MfSigner *signer, const char *sig_namespace, GBytes *data, GError **error)
{
    MfSignerPrivate *priv = GET_PRIVATE(signer);
    g_autofree char *tmp_dir = NULL;
    g_autofree char *key_file = NULL;
    GBytes *signature;

    g_return_val_if_fail(MF_IS_SIGNER(signer), NULL);
    g_return_val_if_fail(sig_namespace != NULL, NULL);
    g_return_val_if_fail(data != NULL, NULL);

    if (priv->key_file != NULL)
        return mf_signer_run_ssh_keygen(signer, priv->key_file, sig_namespace, data, error);

    /* ssh-keygen only reads keys from files, so we write the key to a private
     * directory for as long as it runs */
    tmp_dir = g_dir_make_tmp("manifold-sign-XXXXXX", error);
    if (tmp_dir == NULL)
        return NULL;
    if (priv->key_data != NULL) {
        key_file = g_build_filename(tmp_dir, "key", NULL);
        if (!g_file_set_contents_full(key_file, priv->key_data, -1, G_FILE_SET_CONTENTS_NONE, 0600, error)) {
            g_rmdir(tmp_dir);
            return NULL;
        }
    } else {
        /* with only the public key, ssh-keygen asks ssh-agent to sign */
        g_autofree char *contents = g_strconcat(priv->public_key, "\n", NULL);

        key_file = g_build_filename(tmp_dir, "key" PUBLIC_KEY_SUFFIX, NULL);
        if (!g_file_set_contents_full(key_file, contents, -1, G_FILE_SET_CONTENTS_NONE, 0600, error)) {
            g_rmdir(tmp_dir);
            return NULL;
        }
    }

    signature = mf_signer_run_ssh_keygen(signer, key_file, sig_namespace, data, error);
    g_unlink(key_file);
    g_rmdir(tmp_dir);

    return signature;
}
