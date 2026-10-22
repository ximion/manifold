/*
 * Copyright (C) 2024-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <sys/stat.h>
#include <glib/gstdio.h>

#include "mfr-capture.h"
#include "mft-sandbox.h"
#include "mft-utils.h"

/*
 * Tests of the cache of captured host libraries, in the sandbox: which
 * captures the compose daemon keeps when it starts, and which it removes.
 *
 * Making a capture needs a mounted core, so the tests write the files of a
 * capture directly, as the daemon would leave them, but without any captured
 * libraries.
 */

#define TEST_CAPTURES_DIR MF_RUN_CACHE_DIR "/capture"

/**
 * mft_dir_entry:
 *
 * Make the stamp entry of a host directory, with the current modification
 * time of the directory.
 */
static char *
mft_dir_entry(const char *path)
{
    struct stat st;
    guint64 mtime = 0;

    if (stat(path, &st) == 0)
        mtime = (guint64) st.st_mtim.tv_sec * G_USEC_PER_SEC * 1000 + (guint64) st.st_mtim.tv_nsec;
    return g_strdup_printf("{\"path\": \"%s\", \"mtime\": %" G_GUINT64_FORMAT "}", path, mtime);
}

static char *
mft_write_capture(const char *name, const char *dirs_json, const char *targets_json)
{
    char *dir = g_build_filename(TEST_CAPTURES_DIR, name, NULL);
    g_autofree char *overrides = g_build_filename(dir, "overrides", NULL);
    g_autofree char *stamp_path = g_build_filename(dir, "stamp", NULL);
    g_autofree char *environment_path = g_build_filename(dir, "environment.json", NULL);
    /* clang-format off */
    g_autofree char *stamp = g_strdup_printf(
        "{\n"
        "   \"id\": \"%s\",\n"
        "   \"tuple\": \"x86_64-linux-gnu\",\n"
        "   \"dirs\": %s,\n"
        "   \"targets\": %s\n"
        "}",
        name,
        dirs_json,
        targets_json);
    /* clang-format on */

    mft_make_dirs(overrides);
    mft_write_file(stamp_path, stamp);
    mft_write_file(environment_path, "{\"VK_DRIVER_FILES\": \"\"}");
    return dir;
}

/* the tests share no state; every one starts afresh */
typedef struct {
    gpointer unused;
} Fixture;

static void
mft_setup(Fixture *fixture, gconstpointer user_data)
{
    mft_rm_rf(TEST_CAPTURES_DIR);
    mft_make_dirs(TEST_CAPTURES_DIR);
}

/**
 * test_check_all:
 */
static void
test_check_all(Fixture *fixture, gconstpointer user_data)
{
    g_autofree char *usr_lib = mft_dir_entry("/usr/lib");
    g_autofree char *fresh_dirs = g_strdup_printf("[%s]", usr_lib);
    g_autofree char *fresh = mft_write_capture("fresh", fresh_dirs, "[]");
    g_autofree char *stale_dir = mft_write_capture("stale-dir",
                                                   "[{\"path\": \"/usr/lib\", \"mtime\": 1}]",
                                                   "[]");
    g_autofree char *stale_target = mft_write_capture(
        "stale-target",
        fresh_dirs,
        "[{\"path\": \"/usr/lib/does-not-exist.so\", \"size\": 1, \"mtime\": 1}]");
    g_autofree char *gone_target = mft_write_capture(
        "gone-target",
        fresh_dirs,
        "[{\"path\": \"/usr/lib/does-not-exist.so\", \"size\": 1, \"mtime\": 0}]");
    g_autofree char *no_stamp = mft_write_capture("no-stamp", "[]", "[]");
    g_autofree char *no_stamp_path = g_build_filename(no_stamp, "stamp", NULL);
    g_autofree char *not_a_stamp = mft_write_capture("not-a-stamp", "[]", "[]");
    g_autofree char *not_a_stamp_path = g_build_filename(not_a_stamp, "stamp", NULL);
    g_autofree char *unfinished = g_build_filename(TEST_CAPTURES_DIR, ".tmp-unfinished", NULL);
    g_autofree char *moved_aside = mft_write_capture(".stale-moved-aside", fresh_dirs, "[]");

    g_assert_cmpint(g_unlink(no_stamp_path), ==, 0);
    mft_write_file(not_a_stamp_path, "{\"id\": \"x\"}");
    g_assert_cmpint(g_mkdir(unfinished, 0755), ==, 0);

    mfr_capture_check_all(NULL);

    g_assert_true(g_file_test(fresh, G_FILE_TEST_IS_DIR));
    /* a target that was missing when the capture was made (mtime 0), and is still missing,
     * does not make the capture stale */
    g_assert_true(g_file_test(gone_target, G_FILE_TEST_IS_DIR));
    g_assert_false(g_file_test(stale_dir, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(stale_target, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(no_stamp, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(not_a_stamp, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(unfinished, G_FILE_TEST_EXISTS));
    /* a capture that was moved aside, and that nothing uses anymore */
    g_assert_false(g_file_test(moved_aside, G_FILE_TEST_EXISTS));
}

/**
 * test_discard_all:
 *
 * While the daemon runs, the captures are only moved aside: a tree made with
 * one may be on its way to a runner. They are deleted when the daemon starts.
 */
static void
test_discard_all(Fixture *fixture, gconstpointer user_data)
{
    g_autofree char *usr_lib = mft_dir_entry("/usr/lib");
    g_autofree char *dirs = g_strdup_printf("[%s]", usr_lib);
    g_autofree char *one = mft_write_capture("one", dirs, "[]");
    g_autofree char *two = mft_write_capture("two", dirs, "[]");
    g_autofree char *one_aside = g_build_filename(TEST_CAPTURES_DIR, ".stale-one", NULL);
    g_autofree char *two_aside = g_build_filename(TEST_CAPTURES_DIR, ".stale-two", NULL);
    g_autoptr(GError) error = NULL;

    g_assert_true(mfr_capture_discard_all(&error));
    g_assert_no_error(error);
    g_assert_false(g_file_test(one, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(two, G_FILE_TEST_EXISTS));
    g_assert_true(g_file_test(one_aside, G_FILE_TEST_IS_DIR));
    g_assert_true(g_file_test(two_aside, G_FILE_TEST_IS_DIR));

    mfr_capture_check_all(NULL);
    g_assert_false(g_file_test(one_aside, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(two_aside, G_FILE_TEST_EXISTS));
    g_assert_true(g_file_test(TEST_CAPTURES_DIR, G_FILE_TEST_IS_DIR));
}

/**
 * main:
 */
int
main(int argc, char **argv)
{
    mft_sandbox_enter();
    g_test_init(&argc, &argv, NULL);

#define add_test(name, func) g_test_add("/capture/" name, Fixture, NULL, mft_setup, func, NULL)
    add_test("check-all", test_check_all);
    add_test("discard-all", test_discard_all);
#undef add_test

    return g_test_run();
}
