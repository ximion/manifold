/*
 * Copyright (C) 2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <glib/gstdio.h>
#include <gio/gio.h>
#include <manifold.h>

#include "mft-utils.h"

typedef struct {
    char *dir;
} Fixture;

/**
 * mft_fingerprint_of:
 *
 * Returns: (transfer full): the fingerprint ssh-keygen shows for a key
 */
static char *
mft_fingerprint_of(const char *path)
{
    const char *argv[] = {"ssh-keygen", "-l", "-f", path, NULL};
    g_autoptr(GSubprocess) proc = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree char *output = NULL;
    g_auto(GStrv) parts = NULL;

    proc = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE, &error);
    g_assert_no_error(error);
    g_subprocess_communicate_utf8(proc, NULL, NULL, &output, NULL, &error);
    g_assert_no_error(error);

    /* "<bits> <fingerprint> <comment> (<type>)" */
    parts = g_strsplit(output, " ", 3);
    return g_strdup(parts[1]);
}

/**
 * mft_sign_and_check:
 *
 * Sign some data with @signer, and check the signature.
 */
static void
mft_sign_and_check(Fixture *fixture, MfSigner *signer)
{
    g_autoptr(GBytes) data = g_bytes_new_static("some data\n", 10);
    g_autoptr(GBytes) signature = NULL;
    g_autoptr(GError) error = NULL;

    g_assert_true(mf_signer_check(signer, &error));
    g_assert_no_error(error);
    signature = mf_signer_sign(signer, MF_SIGNATURE_NAMESPACE_BUNDLE, data, &error);
    g_assert_no_error(error);
    g_assert_nonnull(signature);
    mft_check_signature(fixture->dir, data, signature);
}

static void
mft_setup(Fixture *fixture, gconstpointer user_data)
{
    g_autofree char *ssh_keygen = g_find_program_in_path("ssh-keygen");

    if (ssh_keygen == NULL) {
        g_test_skip("ssh-keygen is not installed");
        return;
    }
    fixture->dir = mft_make_tmp_dir("signer");
    /* we must never see the keys of whoever runs the tests */
    g_unsetenv("SSH_AUTH_SOCK");
    g_unsetenv(MF_SIGNING_KEY_ENV);
}

static void
mft_teardown(Fixture *fixture, gconstpointer user_data)
{
    g_autofree char *key_dir = mf_signer_build_key_dir();

    g_unsetenv("SSH_AUTH_SOCK");
    g_unsetenv(MF_SIGNING_KEY_ENV);
    if (fixture->dir != NULL)
        mft_rm_rf(fixture->dir);
    if (g_file_test(key_dir, G_FILE_TEST_EXISTS))
        mft_rm_rf(key_dir);
    g_free(fixture->dir);
}

static void
test_key_file(Fixture *fixture, gconstpointer user_data)
{
    g_autofree char *key = NULL;
    g_autofree char *public_key_file = NULL;
    g_autofree char *public_key = NULL;
    g_autofree char *fingerprint = NULL;
    g_autoptr(MfSigner) signer = NULL;

    if (fixture->dir == NULL)
        return;
    key = mft_make_key(fixture->dir, "key");
    public_key_file = g_strconcat(key, ".pub", NULL);
    public_key = g_strstrip(mft_read_file(public_key_file));
    fingerprint = mft_fingerprint_of(public_key_file);

    signer = mf_signer_new_for_key_file(key);
    g_assert_cmpstr(mf_signer_get_name(signer), ==, key);
    g_assert_cmpstr(mf_signer_get_public_key(signer), ==, public_key);
    g_assert_cmpstr(mf_signer_get_fingerprint(signer), ==, fingerprint);
    mft_sign_and_check(fixture, signer);
}

static void
test_key_data(Fixture *fixture, gconstpointer user_data)
{
    g_autofree char *key = NULL;
    g_autofree char *private_key = NULL;
    g_autoptr(MfSigner) signer = NULL;

    if (fixture->dir == NULL)
        return;
    key = mft_make_key(fixture->dir, "key");
    /* environment variables often lose the final newline */
    private_key = g_strchomp(mft_read_file(key));

    signer = mf_signer_new_for_key_data(private_key);
    g_assert_null(mf_signer_get_name(signer));
    g_assert_null(mf_signer_get_fingerprint(signer));
    mft_sign_and_check(fixture, signer);
}

static void
test_bad_key(Fixture *fixture, gconstpointer user_data)
{
    g_autofree char *key = NULL;
    g_autofree char *missing = NULL;
    g_autoptr(MfSigner) signer = NULL;
    g_autoptr(MfSigner) missing_signer = NULL;
    g_autoptr(GBytes) data = g_bytes_new_static("x", 1);
    g_autoptr(GBytes) signature = NULL;
    g_autoptr(GError) error = NULL;

    if (fixture->dir == NULL)
        return;
    key = g_build_filename(fixture->dir, "key", NULL);
    g_assert_true(g_file_set_contents_full(key, "not a key\n", -1, G_FILE_SET_CONTENTS_NONE, 0600, NULL));

    signer = mf_signer_new_for_key_file(key);
    g_assert_true(mf_signer_check(signer, &error));
    g_assert_no_error(error);
    signature = mf_signer_sign(signer, MF_SIGNATURE_NAMESPACE_BUNDLE, data, &error);
    g_assert_error(error, MF_ERROR, MF_ERROR_FAILED);
    g_assert_null(signature);
    g_clear_error(&error);

    missing = g_build_filename(fixture->dir, "missing", NULL);
    missing_signer = mf_signer_new_for_key_file(missing);
    g_assert_false(mf_signer_check(missing_signer, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_NOT_FOUND);
}

static void
test_key_selection(Fixture *fixture, gconstpointer user_data)
{
    g_autofree char *key_dir = mf_signer_build_key_dir();
    g_autofree char *default_key = NULL;
    g_autofree char *other_key = NULL;
    g_autofree char *private_key = NULL;
    g_autoptr(GPtrArray) keys = NULL;
    g_autoptr(GError) error = NULL;
    MfSigner *signer;

    if (fixture->dir == NULL)
        return;

    /* nothing is selected yet */
    signer = mf_signer_new_default(&error);
    g_assert_error(error, MF_ERROR, MF_ERROR_NOT_FOUND);
    g_assert_null(signer);
    g_clear_error(&error);
    keys = mf_signer_list_keys(&error);
    g_assert_no_error(error);
    g_assert_cmpint(keys->len, ==, 0);
    g_clear_pointer(&keys, g_ptr_array_unref);

    other_key = mft_make_key(key_dir, "other");
    default_key = mft_make_key(key_dir, MF_SIGNING_KEY_DEFAULT_NAME);

    /* the keys of the user are found by name, sorted */
    keys = mf_signer_list_keys(&error);
    g_assert_no_error(error);
    g_assert_cmpint(keys->len, ==, 2);
    g_assert_cmpstr(mf_signer_get_name(g_ptr_array_index(keys, 0)), ==, MF_SIGNING_KEY_DEFAULT_NAME);
    g_assert_cmpstr(mf_signer_get_name(g_ptr_array_index(keys, 1)), ==, "other");
    g_assert_nonnull(mf_signer_get_fingerprint(g_ptr_array_index(keys, 1)));

    signer = mf_signer_new_default(&error);
    g_assert_no_error(error);
    g_assert_cmpstr(mf_signer_get_name(signer), ==, MF_SIGNING_KEY_DEFAULT_NAME);
    mft_sign_and_check(fixture, signer);
    g_object_unref(signer);

    signer = mf_signer_new_for_key("other", &error);
    g_assert_no_error(error);
    g_assert_cmpstr(mf_signer_get_name(signer), ==, "other");
    g_object_unref(signer);

    signer = mf_signer_new_for_key(other_key, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(mf_signer_get_name(signer), ==, other_key);
    g_object_unref(signer);

    /* the environment wins over the default key */
    private_key = mft_read_file(other_key);
    g_setenv(MF_SIGNING_KEY_ENV, private_key, TRUE);
    signer = mf_signer_new_default(&error);
    g_assert_no_error(error);
    g_assert_cmpstr(mf_signer_get_name(signer), ==, "$" MF_SIGNING_KEY_ENV);
    mft_sign_and_check(fixture, signer);
    g_object_unref(signer);

    signer = mf_signer_new_for_key("missing", &error);
    g_assert_error(error, MF_ERROR, MF_ERROR_NOT_FOUND);
    g_assert_null(signer);
    g_clear_error(&error);

    signer = mf_signer_new_for_key("./missing", &error);
    g_assert_error(error, MF_ERROR, MF_ERROR_NOT_FOUND);
    g_assert_null(signer);
    g_clear_error(&error);

    signer = mf_signer_new_for_key("SHA256:nope", &error);
    g_assert_error(error, MF_ERROR, MF_ERROR_NOT_FOUND);
    g_assert_null(signer);
}

static void
test_agent(Fixture *fixture, gconstpointer user_data)
{
    g_autofree char *ssh_agent = NULL;
    g_autofree char *socket = NULL;
    g_autofree char *key = NULL;
    g_autofree char *public_key_file = NULL;
    g_autofree char *fingerprint = NULL;
    g_autoptr(GSubprocess) agent = NULL;
    g_autoptr(GPtrArray) keys = NULL;
    g_autoptr(MfSigner) signer = NULL;
    g_autoptr(GError) error = NULL;

    if (fixture->dir == NULL)
        return;
    ssh_agent = g_find_program_in_path("ssh-agent");
    if (ssh_agent == NULL) {
        g_test_skip("ssh-agent is not installed");
        return;
    }

    /* our own agent, which we point to before anything talks to an agent */
    socket = g_build_filename(fixture->dir, "agent", NULL);
    g_setenv("SSH_AUTH_SOCK", socket, TRUE);
    agent = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE,
                             &error,
                             "ssh-agent",
                             "-D",
                             "-a",
                             socket,
                             NULL);
    g_assert_no_error(error);
    for (guint i = 0; i < 100 && !g_file_test(socket, G_FILE_TEST_EXISTS); i++)
        g_usleep(50 * G_TIME_SPAN_MILLISECOND);
    g_assert_true(g_file_test(socket, G_FILE_TEST_EXISTS));

    key = mft_make_key(fixture->dir, "key");
    mft_run(NULL, (const char *[]){"ssh-add", "-q", key, NULL});
    public_key_file = g_strconcat(key, ".pub", NULL);
    fingerprint = mft_fingerprint_of(public_key_file);
    /* only the agent can sign now */
    g_assert_cmpint(g_unlink(key), ==, 0);

    keys = mf_signer_list_keys(&error);
    g_assert_no_error(error);
    g_assert_cmpint(keys->len, ==, 1);
    g_assert_cmpstr(mf_signer_get_fingerprint(g_ptr_array_index(keys, 0)), ==, fingerprint);

    signer = mf_signer_new_for_key(fingerprint, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(mf_signer_get_name(signer), ==, fingerprint);
    mft_sign_and_check(fixture, signer);

    g_subprocess_force_exit(agent);
    g_subprocess_wait(agent, NULL, NULL);
}

int
main(int argc, char **argv)
{
    /* the key directory of the user is in a directory of the test */
    g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);

#define add_test(name, func) g_test_add("/signer/" name, Fixture, NULL, mft_setup, func, mft_teardown)
    add_test("key-file", test_key_file);
    add_test("key-data", test_key_data);
    add_test("bad-key", test_bad_key);
    add_test("key-selection", test_key_selection);
    add_test("agent", test_agent);
#undef add_test

    return g_test_run();
}
