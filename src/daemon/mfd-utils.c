/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mfd-utils.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libglnx.h>

gboolean
mfd_is_directory(const char *path)
{
    return g_file_test(path, G_FILE_TEST_IS_DIR);
}

gboolean
mfd_is_symlink(const char *path)
{
    return g_file_test(path, G_FILE_TEST_IS_SYMLINK);
}

gboolean
mfd_is_hidden(const char *name)
{
    return name[0] == '.';
}

/**
 * mfd_remove_if_exists:
 *
 * Remove a file, a link or a directory with everything in it.
 */
gboolean
mfd_remove_if_exists(const char *path, GError **error)
{
    return glnx_shutil_rm_rf_at(AT_FDCWD, path, NULL, error);
}

/**
 * mfd_make_dir:
 */
gboolean
mfd_make_dir(const char *path, GError **error)
{
    if (g_mkdir_with_parents(path, 0755) < 0) {
        glnx_throw_errno_prefix(error, "Unable to create directory %s", path);
        return FALSE;
    }
    return TRUE;
}

/**
 * mfd_make_symlink:
 */
gboolean
mfd_make_symlink(const char *target, const char *path, GError **error)
{
    if (symlink(target, path) < 0) {
        glnx_throw_errno_prefix(error, "Unable to create link %s", path);
        return FALSE;
    }
    return TRUE;
}

/**
 * mfd_replace_link:
 *
 * Create a link at a path, replacing what is there, so that there is a
 * valid link at the path at every moment.
 */
gboolean
mfd_replace_link(const char *target, const char *dir, const char *name, GError **error)
{
    g_autofree char *link_path = g_build_filename(dir, name, NULL);
    g_autofree char *tmp_name = g_strconcat(MF_TMP_PREFIX, name, NULL);
    g_autofree char *tmp_path = g_build_filename(dir, tmp_name, NULL);

    if (!mfd_remove_if_exists(tmp_path, error))
        return FALSE;
    if (!mfd_make_symlink(target, tmp_path, error))
        return FALSE;
    if (rename(tmp_path, link_path) < 0) {
        glnx_throw_errno_prefix(error, "Unable to rename %s", tmp_path);
        unlink(tmp_path);
        return FALSE;
    }

    return TRUE;
}

/**
 * mfd_write_marker:
 *
 * Write a small file of a component directory, replacing what is there.
 */
gboolean
mfd_write_marker(const char *dir, const char *name, const char *content, gssize length, GError **error)
{
    g_autofree char *path = g_build_filename(dir, name, NULL);

    return g_file_set_contents_full(path, content, length, G_FILE_SET_CONTENTS_CONSISTENT, 0644, error);
}

/**
 * mfd_write_word:
 *
 * Write a marker file that holds one word.
 */
gboolean
mfd_write_word(const char *dir, const char *name, const char *word, GError **error)
{
    g_autofree char *line = g_strconcat(word, "\n", NULL);

    return mfd_write_marker(dir, name, line, -1, error);
}
