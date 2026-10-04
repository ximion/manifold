/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define _GNU_SOURCE

#include "config.h"
#include "mft-sandbox.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * The sandbox that tests of privileged components run in.
 *
 * Manifold's daemons use paths that are fixed at build time, and write to
 * directories only root can write to. Instead of teaching them to behave
 * differently when tested, the tests change what these paths refer to: They
 * run in a private mount namespace, in which the directories Manifold keeps
 * its state in are empty tmpfs mounts. Nothing of this is visible outside
 * of the sandbox, and nothing is left behind when its last process exits.
 *
 * A state directory may not exist on the host, and its parent belongs to the
 * host: such a parent is replaced by a tmpfs holding bind mounts of everything
 * that was in it, so that the state directory can be created in it.
 */

static const char *const STATE_DIRS[] = {MF_RUNTIME_DIR, MF_STORAGE_DIR, MF_CACHE_DIR};

static void
mft_skip(const char *what, int err)
{
    g_printerr("SKIP: Unable to %s: %s\n", what, g_strerror(err));
    exit(MFT_EXIT_SKIPPED);
}

/**
 * mft_fail_unless_denied:
 *
 * A failure of something the host may not allow is a reason to skip, any
 * other failure is a bug.
 */
static void
mft_fail_unless_denied(const char *what)
{
    int err = errno;

    if (err == EPERM || err == EACCES || err == ENOSYS || err == EINVAL)
        mft_skip(what, err);
    g_error("Unable to %s: %s", what, g_strerror(err));
}

static void
mft_write_proc_file(const char *name, const char *content)
{
    g_autofree char *path = g_build_filename("/proc/self", name, NULL);
    int fd = open(path, O_WRONLY | O_CLOEXEC);

    if (fd < 0 || write(fd, content, strlen(content)) != (ssize_t) strlen(content))
        mft_fail_unless_denied("set up the user namespace");
    close(fd);
}

/**
 * mft_enter_user_namespace:
 *
 * Become root in a new user namespace, with a new mount namespace.
 */
static void
mft_enter_user_namespace(void)
{
    g_autofree char *uid_map = g_strdup_printf("0 %u 1", (unsigned int) getuid());
    g_autofree char *gid_map = g_strdup_printf("0 %u 1", (unsigned int) getgid());

    if (unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0)
        mft_fail_unless_denied("create a user namespace");
    mft_write_proc_file("setgroups", "deny");
    mft_write_proc_file("uid_map", uid_map);
    mft_write_proc_file("gid_map", gid_map);
}

static void
mft_mount_tmpfs(const char *path)
{
    if (mount("tmpfs", path, "tmpfs", 0, "mode=0755") != 0)
        g_error("Unable to mount a tmpfs on %s: %s", path, g_strerror(errno));
}

/**
 * mft_make_writable:
 *
 * Replace a directory of the host by one with the same entries, which we can
 * add to: a tmpfs, in which every entry of the original is bind mounted. The
 * original is hidden by the tmpfs, but still reachable through a descriptor.
 */
static void
mft_make_writable(const char *dir)
{
    DIR *entries = opendir(dir);
    struct dirent *entry;

    if (entries == NULL)
        g_error("Unable to open %s: %s", dir, g_strerror(errno));
    mft_mount_tmpfs(dir);

    while ((entry = readdir(entries)) != NULL) {
        g_autofree char *source = g_strdup_printf("/proc/self/fd/%d/%s", dirfd(entries), entry->d_name);
        g_autofree char *target = g_build_filename(dir, entry->d_name, NULL);
        struct stat st;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        /* an entry may go away in the meantime, which is common below /run */
        if (fstatat(dirfd(entries), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0)
            continue;

        if (S_ISLNK(st.st_mode)) {
            g_autofree char *link_target = g_file_read_link(source, NULL);

            if (link_target != NULL && symlink(link_target, target) != 0)
                g_error("Unable to create link %s: %s", target, g_strerror(errno));
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            if (mkdir(target, 0755) != 0)
                g_error("Unable to create mount point %s: %s", target, g_strerror(errno));
        } else {
            int fd = open(target, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);

            if (fd < 0)
                g_error("Unable to create mount point %s: %s", target, g_strerror(errno));
            close(fd);
        }
        if (mount(source, target, NULL, MS_BIND | MS_REC, NULL) != 0 && errno != ENOENT)
            g_error("Unable to bind %s: %s", target, g_strerror(errno));
    }

    closedir(entries);
}

/**
 * mft_mount_state_dir:
 *
 * Replace a state directory by an empty tmpfs, creating it if the host does
 * not have it.
 */
static void
mft_mount_state_dir(const char *path)
{
    if (!g_file_test(path, G_FILE_TEST_IS_DIR)) {
        g_autofree char *parent = g_path_get_dirname(path);

        mft_make_writable(parent);
        if (mkdir(path, 0755) != 0)
            g_error("Unable to create %s: %s", path, g_strerror(errno));
    }
    mft_mount_tmpfs(path);
}

/**
 * mft_sandbox_enter:
 *
 * Move this process into a sandbox of its own. Exits with the status Meson
 * takes as "skipped" if the host does not allow it, and aborts if anything
 * else goes wrong.
 */
void
mft_sandbox_enter(void)
{
    mft_enter_user_namespace();

    /* changes to mounts must neither leave, nor enter, the sandbox */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0)
        mft_fail_unless_denied("make mounts private");

    for (size_t i = 0; i < G_N_ELEMENTS(STATE_DIRS); i++)
        mft_mount_state_dir(STATE_DIRS[i]);
}
