/*
 * Copyright (C) 2024-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define _GNU_SOURCE
#include "config.h"
#include "mfr-tree.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <libglnx.h>
#include <systemd/sd-json.h>

#include "mf-json-private.h"
#include "mf-sdautoptr-private.h"

constexpr char LAYER_NAME[] = "layer";
constexpr char WORK_NAME[] = "work";
constexpr char ENVIRONMENT_RECORD[] = "run/manifold/environment.json";
constexpr size_t KEY_LENGTH = 32;

/* host paths bind-mounted into every tree, besides /run/host */
static const struct {
    const char *path;
    gboolean read_only;
} HOST_BINDS[] = {
    {"/dev",           FALSE},
    {"/sys",           TRUE },
    {"/home",          FALSE},
    {"/srv",           FALSE},
    {"/mnt",           FALSE},
    {"/run/user",      FALSE},
    {"/run/dbus",      FALSE},
    {"/run/udev",      TRUE },
    {"/run/systemd",   TRUE },
    /* the tree has its own /tmp, and only needs the X11 sockets of the host there */
    {"/tmp/.X11-unix", FALSE},
};

/* host paths bind-mounted below /run/host, besides the root filesystem */
static const char *const HOST_ROOT_BINDS[] = {"/usr", "/etc"};

/* the name-service and site configuration of the host, which replaces the image's */
static const char *const HOST_ETC_FILES[] = {
    "passwd",
    "group",
    "nsswitch.conf",
    "hosts",
    "host.conf",
    "resolv.conf",
    "localtime",
    "machine-id",
};

/* the procfs options that limit which processes it shows, and to whom */
static const char *const PROC_VISIBILITY_OPTIONS[] = {"hidepid=", "gid=", "subset="};

/**
 * MfrTree:
 * The mount trees applications are launched in.
 *
 * A tree is assembled at MF_RUN_MOUNT_DIR/trees/<key>/ in the mount
 * namespace of the daemon, and stays there until it is torn down. Launching
 * an application hands out a clone of it. The key is a hash of the image IDs
 * and the capture it is comprised of, so a tree can be reused as long as these
 * layers still are what the application's runtime consists of.
 *
 * The image mounts come from the image mount cache #MfrImageMounts which handles
 * mounting them. The capture comes from the index of captures, which #MfrComposer
 * holds and passes to every new #MfrTree at construction. The tree
 * itself only mounts the overlays and standard mounts.
 *
 * Besides the mounts, a tree has a directory of its own below
 * MF_RUN_CACHE_DIR/trees/<key>/, holding the layer that is specific to the
 * tree: the ld.so.cache for the layers and the capture, the record of what
 * was assembled, and the directories the host is bound at.
 */
struct _MfrTree {
    GObject parent_instance;

    char *key;
    char *mount_path; /* below MFR_TREES_MOUNT_DIR */
    char *cache_dir;  /* below MFR_TREES_CACHE_DIR */
    char *executable; /* the program to run, absolute within the tree */
    MfrApp *app;
    MfrCapture *capture;
    MfrImageMounts *img_mounts; /* where the images of the layers are mounted */
    MfrImageMount
        *mounts[MFR_LAYER_COUNT]; /* the mounted images of the layers, NULL for a layer the app lacks */
    char **environment;
    gboolean mounted;
};

G_DEFINE_FINAL_TYPE(MfrTree, mfr_tree, G_TYPE_OBJECT)

static void
mfr_tree_init(MfrTree *tree)
{
}

static void
mfr_tree_finalize(GObject *object)
{
    MfrTree *tree = MFR_TREE(object);

    g_free(tree->key);
    g_free(tree->mount_path);
    g_free(tree->cache_dir);
    g_free(tree->executable);
    g_clear_object(&tree->app);
    g_clear_object(&tree->capture);
    g_clear_object(&tree->img_mounts);
    g_strfreev(tree->environment);

    G_OBJECT_CLASS(mfr_tree_parent_class)->finalize(object);
}

static void
mfr_tree_class_init(MfrTreeClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = mfr_tree_finalize;
}

/**
 * mfr_make_tree_key:
 *
 * Make the key of a tree from the images of its layers and its capture.
 */
static char *
mfr_make_tree_key(const MfrApp *app, const MfrCapture *capture)
{
    g_autoptr(GChecksum) checksum = g_checksum_new(G_CHECKSUM_SHA256);

    for (MfrLayerKind kind = 0; kind < MFR_LAYER_COUNT; kind++) {
        if (app->layers[kind].image.verity_hash != NULL)
            g_checksum_update(checksum, (const guchar *) app->layers[kind].image.verity_hash, -1);
        g_checksum_update(checksum, (const guchar *) ":", 1);
    }
    g_checksum_update(checksum, (const guchar *) mfr_capture_get_id(capture), -1);

    return g_strndup(g_checksum_get_string(checksum), KEY_LENGTH);
}

/**
 * mfr_set_mount_attributes:
 *
 * Set our mount attributes on a directory.
 */
static gboolean
mfr_set_mount_attributes(const char *target, gboolean read_only, GError **error)
{
    struct mount_attr attr = {
        /* nothing bound from the host may grant privileges in a tree */
        .attr_set = MOUNT_ATTR_NOSUID | (read_only ? MOUNT_ATTR_RDONLY : 0),
    };

    if (mount_setattr(AT_FDCWD, target, AT_RECURSIVE, &attr, sizeof(attr)) != 0)
        return glnx_throw_errno_prefix(error, "Unable to set attributes of %s", target);
    return TRUE;
}

/**
 * mfr_bind_mount:
 *
 * Bind mount @source at @target, and set mount attributes.
 */
static gboolean
mfr_bind_mount(const char *source, const char *target, gboolean read_only, GError **error)
{
    if (mount(source, target, NULL, MS_BIND | MS_REC, NULL) != 0)
        return glnx_throw_errno_prefix(error, "Unable to bind %s at %s", source, target);
    return mfr_set_mount_attributes(target, read_only, error);
}

/**
 * mfr_bind_host:
 *
 * The host filesystem is mounted at /run/host in a tree, read-only.
 * We mount its root filesystem alone, without anything mounted below it,
 * so that no mount of the host (and none of our trees), can appear in there.
 * Then we explicitly mount /usr and /etc, which may be mounts of their own,
 * and which is what the captured links and ldconfig need.
 */
static gboolean
mfr_bind_host(const char *tree_path, GError **error)
{
    g_autofree char *host_dir = g_build_filename(tree_path, MFR_HOST_DIR, NULL);

    if (mount("/", host_dir, NULL, MS_BIND, NULL) != 0)
        return glnx_throw_errno_prefix(error, "Unable to bind the host root at %s", host_dir);
    for (size_t i = 0; i < G_N_ELEMENTS(HOST_ROOT_BINDS); i++) {
        g_autofree char *target = g_build_filename(host_dir, HOST_ROOT_BINDS[i], NULL);

        if (mount(HOST_ROOT_BINDS[i], target, NULL, MS_BIND | MS_REC, NULL) != 0)
            return glnx_throw_errno_prefix(error, "Unable to bind %s at %s", HOST_ROOT_BINDS[i], target);
    }

    return mfr_set_mount_attributes(host_dir, TRUE, error);
}

/**
 * mfr_mount_overlay:
 *
 * Mount an overlayfs with the given flags to @target.
 */
static gboolean
mfr_mount_overlay(const char *target, const char *options, unsigned long flags, GError **error)
{
    if (mount("overlay", target, "overlay", flags, options) != 0)
        return glnx_throw_errno_prefix(error, "Unable to mount overlay at %s", target);
    return TRUE;
}

/**
 * mfr_mountinfo_get_super_options:
 * @line: a line of a mountinfo file
 * @fs_type: the filesystem type the mount must have
 * @mount_point: (nullable): where the mount must be, as the file escapes it, or %NULL for anywhere
 *
 * Returns: (transfer full) (nullable): the superblock options of the mount
 *   in @line, or %NULL if it is not one of @fs_type at @mount_point
 */
static char *
mfr_mountinfo_get_super_options(const char *line, const char *fs_type, const char *mount_point)
{
    /* the mount point is the fifth field; after the separator follow the
     * filesystem type, the source, and the superblock options */
    const char *separator = strstr(line, " - ");
    g_auto(GStrv) mount_fields = g_strsplit(line, " ", 6);
    g_auto(GStrv) fs_fields = NULL;

    if (separator == NULL || g_strv_length(mount_fields) < 6)
        return NULL;
    if (mount_point != NULL && !g_str_equal(mount_fields[4], mount_point))
        return NULL;
    fs_fields = g_strsplit(separator + strlen(" - "), " ", 3);
    if (g_strv_length(fs_fields) < 3 || !g_str_equal(fs_fields[0], fs_type))
        return NULL;
    return g_strdup(fs_fields[2]);
}

/**
 * mfr_read_host_proc_options:
 *
 * Read the options of the host's procfs that limit which processes it shows,
 * and to whom.
 *
 * Returns: (transfer full) (nullable): the options, separated by commas, or
 *   %NULL on error
 */
static char *
mfr_read_host_proc_options(GError **error)
{
    g_autofree char *mountinfo = NULL;
    g_autofree char *super_options = NULL;
    g_auto(GStrv) lines = NULL;
    g_auto(GStrv) all_options = NULL;
    g_autoptr(GString) options = g_string_new(NULL);

    if (!g_file_get_contents("/proc/self/mountinfo", &mountinfo, NULL, error))
        return NULL;
    lines = g_strsplit(mountinfo, "\n", -1);
    /* a later mount at /proc is on top of the earlier ones */
    for (size_t i = 0; lines[i] != NULL; i++) {
        char *line_options = mfr_mountinfo_get_super_options(lines[i], "proc", "/proc");

        if (line_options != NULL) {
            g_free(super_options);
            super_options = line_options;
        }
    }
    if (super_options == NULL) {
        glnx_throw(error, "The host has no procfs at /proc");
        return NULL;
    }

    all_options = g_strsplit(super_options, ",", -1);
    for (size_t i = 0; all_options[i] != NULL; i++) {
        for (size_t j = 0; j < G_N_ELEMENTS(PROC_VISIBILITY_OPTIONS); j++) {
            if (!g_str_has_prefix(all_options[i], PROC_VISIBILITY_OPTIONS[j]))
                continue;
            if (options->len > 0)
                g_string_append_c(options, ',');
            g_string_append(options, all_options[i]);
        }
    }

    return g_string_free_and_steal(g_steal_pointer(&options));
}

/**
 * mfr_build_environment_vars:
 *
 * Build the environment variables that a launch sets or overrides.
 */
static char **
mfr_build_environment_vars(const MfrCapture *capture)
{
    g_autoptr(GStrvBuilder) builder = g_strv_builder_new();

    /* env from our host capture */
    g_strv_builder_addv(builder, (const char **) mfr_capture_get_environment(capture));

    /* switch that sends GLib and GTK to the desktop portals, which work better for us */
    g_strv_builder_add(builder, "GTK_USE_PORTAL=1");

    return g_strv_builder_end(builder);
}

/**
 * mfr_write_environment_record:
 *
 * Write down what the tree was assembled from, for the application that runs
 * in it, and for bug reports.
 */
static gboolean
mfr_write_environment_record(MfrTree *tree, const char *path, GError **error)
{
    g_autoptr(sd_json_variant) record = NULL;
    g_autoptr(sd_json_variant) layers = NULL;
    g_autoptr(sd_json_variant) environment = NULL;
    struct utsname uts = {0};

    int r = mfr_app_append_layers_json(tree->app, &layers);
    if (r < 0)
        return glnx_throw(error, "Unable to build environment record: %s", g_strerror(-r));
    for (size_t i = 0; tree->environment[i] != NULL; i++) {
        g_autofree char *name = g_strdup(tree->environment[i]);
        char *value = strchr(name, '=');

        if (value == NULL)
            continue;
        *value++ = '\0';
        r = sd_json_variant_set_field_string(&environment, name, value);
        if (r < 0)
            return glnx_throw(error, "Unable to build environment record: %s", g_strerror(-r));
    }
    uname(&uts);

    r = sd_json_buildo(&record,
                       SD_JSON_BUILD_PAIR_STRING("componentId", tree->app->component_id),
                       SD_JSON_BUILD_PAIR_STRING("tree", tree->key),
                       SD_JSON_BUILD_PAIR_VARIANT("layers", layers),
                       SD_JSON_BUILD_PAIR_STRING("capture", mfr_capture_get_id(tree->capture)),
                       SD_JSON_BUILD_PAIR_VARIANT("environment", environment),
                       SD_JSON_BUILD_PAIR_STRING("manifold", MF_VERSION));
    if (r < 0)
        return glnx_throw(error, "Unable to build environment record: %s", g_strerror(-r));

    return mf_json_write_file(path, record, error);
}

/**
 * mfr_host_path_is_bound:
 *
 * Check whether a path of the host is at the same path in every tree, as it
 * is one of HOST_BINDS or below one of them.
 */
static gboolean
mfr_host_path_is_bound(const char *path)
{
    for (size_t i = 0; i < G_N_ELEMENTS(HOST_BINDS); i++) {
        size_t length = strlen(HOST_BINDS[i].path);

        if (strncmp(path, HOST_BINDS[i].path, length) == 0 && (path[length] == '\0' || path[length] == '/'))
            return TRUE;
    }

    return FALSE;
}

/**
 * mfr_read_run_link:
 * @path: a file of the host's /etc, or its copy in the layer of a tree
 *
 * Returns: (transfer full) (nullable): the absolute path the link points
 *   to, if it is a link into a directory below /run, otherwise %NULL
 */
static char *
mfr_read_run_link(const char *path)
{
    g_autofree char *target = g_file_read_link(path, NULL);
    g_autofree char *canonical = NULL;

    if (target == NULL)
        return NULL;
    canonical = g_canonicalize_filename(target, "/etc");
    if (!g_str_has_prefix(canonical, "/run/") || strchr(canonical + strlen("/run/"), '/') == NULL)
        return NULL;
    return g_steal_pointer(&canonical);
}

/**
 * mfr_prepare_etc_files:
 *
 * Create placeholders in the tree's layer for the host files listed in
 * HOST_ETC_FILES, which hide the image's versions:
 *  - If the host's file is a link into a directory below /run, recreate the
 *    link, and create a mount point for that directory, unless it is bound
 *    into every tree anyway. mfr_bind_etc_files() binds the directory, so
 *    the link resolves to the current file of the host, even after the host
 *    replaced it (e.g. resolv.conf managed by NetworkManager).
 *  - Otherwise create an empty file, which mfr_bind_etc_files() later
 *    bind-mounts the host's file over.
 *  - If the host does not have the file, create nothing, so the image's
 *    version (if any) stays visible.
 *
 * Entries left by a previous assembly of the reused layer are removed first.
 */
static gboolean
mfr_prepare_etc_files(const char *layer_dir, GError **error)
{
    g_autofree char *etc_dir = g_build_filename(layer_dir, "etc", NULL);

    if (!glnx_shutil_mkdir_p_at(AT_FDCWD, etc_dir, 0755, NULL, error))
        return FALSE;
    for (size_t i = 0; i < G_N_ELEMENTS(HOST_ETC_FILES); i++) {
        g_autofree char *host_path = g_build_filename("/etc", HOST_ETC_FILES[i], NULL);
        g_autofree char *path = g_build_filename(etc_dir, HOST_ETC_FILES[i], NULL);
        g_autofree char *run_target = mfr_read_run_link(host_path);

        /* the layer is reused, so we remove what a previous assembly put here first */
        if (unlink(path) != 0 && errno != ENOENT)
            return glnx_throw_errno_prefix(error, "Unable to remove %s", path);
        if (!g_file_test(host_path, G_FILE_TEST_EXISTS))
            continue;
        if (run_target != NULL) {
            g_autofree char *run_dir = g_path_get_dirname(run_target);
            g_autofree char *mount_point = g_build_filename(layer_dir, run_dir, NULL);

            if (symlink(run_target, path) != 0)
                return glnx_throw_errno_prefix(error, "Unable to link %s", path);
            if (!mfr_host_path_is_bound(run_dir) &&
                !glnx_shutil_mkdir_p_at(AT_FDCWD, mount_point, 0755, NULL, error))
                return FALSE;
        } else if (!g_file_set_contents(path, "", 0, error)) {
            return FALSE;
        }
    }

    return TRUE;
}

/**
 * mfr_bind_etc_files:
 *
 * Bind-mount the host's files over the empty ones of the layer, and the
 * directories the links of the layer point into, read-only.
 */
static gboolean
mfr_bind_etc_files(const char *tree_path, const char *layer_dir, GError **error)
{
    g_autoptr(GHashTable) bound_dirs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    for (size_t i = 0; i < G_N_ELEMENTS(HOST_ETC_FILES); i++) {
        g_autofree char *host_path = g_build_filename("/etc", HOST_ETC_FILES[i], NULL);
        g_autofree char *placeholder = g_build_filename(layer_dir, "etc", HOST_ETC_FILES[i], NULL);
        g_autofree char *target = g_build_filename(tree_path, "etc", HOST_ETC_FILES[i], NULL);
        g_autofree char *run_target = mfr_read_run_link(placeholder);

        if (run_target != NULL) {
            g_autofree char *run_dir = g_path_get_dirname(run_target);
            g_autofree char *dir_target = g_build_filename(tree_path, run_dir, NULL);

            if (mfr_host_path_is_bound(run_dir) || !g_hash_table_add(bound_dirs, g_strdup(run_dir)))
                continue;
            if (!mfr_bind_mount(run_dir, dir_target, TRUE, error))
                return FALSE;
            continue;
        }
        /* mounting at a link would follow it outside of the tree */
        if (g_file_test(placeholder, G_FILE_TEST_IS_SYMLINK) ||
            !g_file_test(placeholder, G_FILE_TEST_IS_REGULAR))
            continue;
        if (!mfr_bind_mount(host_path, target, TRUE, error))
            return FALSE;
    }

    return TRUE;
}

/**
 * mfr_tree_prepare_layer:
 *
 * Prepare the layer of the tree itself: the mount points for the host, the
 * environment record, and later the ld.so.cache.
 */
static gboolean
mfr_tree_prepare_layer(MfrTree *tree, const char *layer_dir, GError **error)
{
    g_autofree char *record_path = g_build_filename(layer_dir, ENVIRONMENT_RECORD, NULL);
    g_autofree char *record_dir = g_path_get_dirname(record_path);
    g_autofree char *host_dir = g_build_filename(layer_dir, MFR_HOST_DIR, NULL);
    g_autofree char *app_dir = g_build_filename(layer_dir, "app", NULL);
    g_autofree char *proc_dir = g_build_filename(layer_dir, "proc", NULL);

    /* every mount point of the tree is a directory of this layer, as one of an
     * image could be a link, which mounting would follow outside of the tree */
    if (!glnx_shutil_mkdir_p_at(AT_FDCWD, record_dir, 0755, NULL, error) ||
        !glnx_shutil_mkdir_p_at(AT_FDCWD, host_dir, 0755, NULL, error) ||
        !glnx_shutil_mkdir_p_at(AT_FDCWD, app_dir, 0755, NULL, error) ||
        !glnx_shutil_mkdir_p_at(AT_FDCWD, proc_dir, 0755, NULL, error))
        return FALSE;

    /* a directory here merges with the one of the images, if any */
    for (size_t i = 0; i < G_N_ELEMENTS(HOST_BINDS); i++) {
        g_autofree char *dir = g_build_filename(layer_dir, HOST_BINDS[i].path, NULL);

        /* below /tmp, the mount point is made on the tree's tmpfs */
        if (g_str_has_prefix(HOST_BINDS[i].path, "/tmp/"))
            dir[strlen(layer_dir) + strlen("/tmp")] = '\0';
        if (!glnx_shutil_mkdir_p_at(AT_FDCWD, dir, 0755, NULL, error))
            return FALSE;
    }

    if (!mfr_prepare_etc_files(layer_dir, error))
        return FALSE;
    return mfr_write_environment_record(tree, record_path, error);
}

/**
 * mfr_tree_run_ldconfig:
 *
 * Regenerate ld.so.cache for the tree, so that the captured libraries are
 * found by their SONAME. The tree is mounted writable for this, with the
 * tree's own layer as the upper directory, which is where the cache lands.
 * The application is at /app meanwhile, as the image's ld.so.conf names
 * /app/lib. ldconfig is the host's; with -r it prefixes every path and
 * does not execute anything from the tree.
 */
static gboolean
mfr_tree_run_ldconfig(MfrTree *tree,
                      const char *lower,
                      const char *layer_dir,
                      const char *work_dir,
                      const char *app_dir,
                      GError **error)
{
    g_autofree char *options = g_strdup_printf("lowerdir=%s,upperdir=%s,workdir=%s",
                                               lower,
                                               layer_dir,
                                               work_dir);
    g_autofree char *app_target = g_build_filename(tree->mount_path, "app", NULL);
    g_autofree char *standard_error = NULL;
    const char *argv[] = {"ldconfig", "-X", "-r", tree->mount_path, NULL};
    gboolean ret = FALSE;
    int wait_status;

    if (!mfr_mount_overlay(tree->mount_path, options, MS_NOSUID | MS_NODEV, error))
        return FALSE;
    /* the captured links point below /run/host, which must resolve */
    if (!mfr_bind_host(tree->mount_path, error) || !mfr_bind_mount(app_dir, app_target, TRUE, error))
        goto out;

    if (!g_spawn_sync(NULL,
                      (char **) argv,
                      NULL,
                      G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL,
                      NULL,
                      NULL,
                      NULL,
                      &standard_error,
                      &wait_status,
                      error))
        goto out;
    if (!g_spawn_check_wait_status(wait_status, error)) {
        glnx_prefix_error(error, "ldconfig failed (%s)", g_strchomp(standard_error));
        goto out;
    }
    ret = TRUE;

out:
    if (umount2(tree->mount_path, MNT_DETACH) != 0)
        g_warning("Unable to unmount %s: %s", tree->mount_path, g_strerror(errno));
    return ret;
}

/**
 * mfr_tree_mount_tree:
 *
 * Mount the tree as applications see it: the read-only overlay of the system
 * layers, with the application's image at /app and the host bound into it.
 * Its procfs is its own, but shows no more processes than the host's.
 */
static gboolean
mfr_tree_mount_tree(MfrTree *tree,
                    const char *lower,
                    const char *layer_dir,
                    const char *app_dir,
                    GError **error)
{
    g_autofree char *options = g_strdup_printf("lowerdir=%s:%s", layer_dir, lower);
    g_autofree char *proc_dir = g_build_filename(tree->mount_path, "proc", NULL);
    g_autofree char *tmp_dir = g_build_filename(tree->mount_path, "tmp", NULL);
    g_autofree char *app_target = g_build_filename(tree->mount_path, "app", NULL);
    g_autofree char *proc_options = mfr_read_host_proc_options(error);

    if (proc_options == NULL)
        return FALSE;
    if (!mfr_mount_overlay(tree->mount_path, options, MS_RDONLY | MS_NOSUID | MS_NODEV, error))
        return FALSE;
    tree->mounted = TRUE;

    /* mount the app into place */
    if (!mfr_bind_mount(app_dir, app_target, TRUE, error))
        return FALSE;

    /* bindmound the host into /run/host */
    if (!mfr_bind_host(tree->mount_path, error))
        return FALSE;

    if (mount("proc", proc_dir, "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, proc_options) != 0)
        return glnx_throw_errno_prefix(error, "Unable to mount /proc at %s", proc_dir);
    /* a /tmp of the tree's own, but we share it across launches */
    if (mount("tmpfs", tmp_dir, "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777") != 0)
        return glnx_throw_errno_prefix(error, "Unable to mount tmpfs at %s", tmp_dir);

    /* bindmount some stuff from the host directly in-place */
    for (size_t i = 0; i < G_N_ELEMENTS(HOST_BINDS); i++) {
        g_autofree char *target = g_build_filename(tree->mount_path, HOST_BINDS[i].path, NULL);

        if (!g_file_test(HOST_BINDS[i].path, G_FILE_TEST_IS_DIR))
            continue;
        /* a mount point below /tmp is made on the tree's tmpfs */
        if (g_str_has_prefix(HOST_BINDS[i].path, "/tmp/") &&
            !glnx_shutil_mkdir_p_at(AT_FDCWD, target, 01777, NULL, error))
            return FALSE;
        if (!mfr_bind_mount(HOST_BINDS[i].path, target, HOST_BINDS[i].read_only, error))
            return FALSE;
    }

    return mfr_bind_etc_files(tree->mount_path, layer_dir, error);
}

/**
 * mfr_tree_release_images:
 *
 * Give the mounts of the images of the tree back to the cache.
 */
static void
mfr_tree_release_images(MfrTree *tree)
{
    for (MfrLayerKind kind = 0; kind < MFR_LAYER_COUNT; kind++) {
        mfr_image_mounts_release(tree->img_mounts, tree->mounts[kind]);
        tree->mounts[kind] = NULL;
    }
}

/**
 * mfr_tree_acquire_images:
 *
 * Mount the image of every layer the application has, or reuse its mount.
 *
 * Returns: %TRUE if every image is mounted
 */
static gboolean
mfr_tree_acquire_images(MfrTree *tree, GError **error)
{
    for (MfrLayerKind kind = 0; kind < MFR_LAYER_COUNT; kind++) {
        const MfrLayer *layer = &tree->app->layers[kind];

        if (layer->image.verity_hash == NULL)
            continue;
        tree->mounts[kind] = mfr_image_mounts_acquire(tree->img_mounts, &layer->image, error);
        if (tree->mounts[kind] == NULL)
            return FALSE;
        g_info("Using %s %s at %s",
               layer->component_id,
               layer->version,
               mfr_image_mount_get_path(tree->mounts[kind]));
    }

    return TRUE;
}

/**
 * mfr_tree_ensure_capture:
 * @captures: (element-type utf8 MfrCapture): the captures by the hash of their core
 *
 * Take the capture for the core of the application from the index, or add
 * one, and make sure it matches the host. The core must be mounted.
 */
static gboolean
mfr_tree_ensure_capture(MfrTree *tree, GHashTable *captures, GError **error)
{
    const char *core_hash = tree->app->layers[MFR_LAYER_CORE].image.verity_hash;
    MfrCapture *capture = g_hash_table_lookup(captures, core_hash);

    if (capture == NULL) {
        capture = mfr_capture_new(core_hash, tree->app->arch_tuple);
        g_hash_table_insert(captures, g_strdup(core_hash), capture);
    }
    if (!mfr_capture_ensure(capture, mfr_image_mount_get_path(tree->mounts[MFR_LAYER_CORE]), error))
        return FALSE;

    tree->capture = g_object_ref(capture);
    return TRUE;
}

/**
 * mfr_tree_teardown:
 *
 * Take a tree down, and let go of its images. Applications that were
 * launched in it keep their clone.
 */
void
mfr_tree_teardown(MfrTree *tree)
{
    if (tree->mounted) {
        if (umount2(tree->mount_path, MNT_DETACH) != 0)
            g_warning("Unable to unmount %s: %s", tree->mount_path, g_strerror(errno));
        tree->mounted = FALSE;
    }
    if (rmdir(tree->mount_path) != 0 && errno != ENOENT)
        g_warning("Unable to remove %s: %s", tree->mount_path, g_strerror(errno));
    mfr_tree_release_images(tree);
}

/**
 * mfr_tree_has_program:
 * @path: the program, absolute within the tree
 *
 * Whether the tree has an executable file at @path. Links are followed as
 * if the tree was the root, as they are once the program runs in it.
 */
static gboolean
mfr_tree_has_program(MfrTree *tree, const char *path)
{
    struct open_how how = {.flags = O_PATH | O_CLOEXEC, .resolve = RESOLVE_IN_ROOT};
    glnx_autofd int root_fd = open(tree->mount_path, O_PATH | O_DIRECTORY | O_CLOEXEC);
    glnx_autofd int fd = root_fd >= 0 ? (int) syscall(SYS_openat2, root_fd, path, &how, sizeof(how)) : -1;
    struct stat st;

    return fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0111) != 0;
}

/**
 * mfr_tree_assemble_internal:
 *
 * Mount a tree, from the images of its layers and its capture.
 */
static gboolean
mfr_tree_assemble_internal(MfrTree *tree, GError **error)
{
    g_autofree char *layer_dir = g_build_filename(tree->cache_dir, LAYER_NAME, NULL);
    g_autofree char *work_dir = g_build_filename(tree->cache_dir, WORK_NAME, NULL);
    g_autofree char *lower = NULL;
    g_autofree char *app_dir = g_build_filename(mfr_image_mount_get_path(tree->mounts[MFR_LAYER_APP]),
                                                "app",
                                                NULL);
    g_autoptr(GPtrArray) lowers = g_ptr_array_new();

    /* overlayfs wants the highest layer first: the overrides, then the runtime of the
     * application. The application itself is no layer, its /app is bound into the tree. */
    g_ptr_array_add(lowers, (char *) mfr_capture_get_overrides_dir(tree->capture));
    for (int kind = MFR_LAYER_APP - 1; kind >= MFR_LAYER_CORE; kind--) {
        if (tree->mounts[kind] != NULL)
            g_ptr_array_add(lowers, (char *) mfr_image_mount_get_path(tree->mounts[kind]));
    }
    g_ptr_array_add(lowers, NULL);

    lower = g_strjoinv(":", (char **) lowers->pdata);
    /* binding /app would follow a link outside of the image */
    if (g_file_test(app_dir, G_FILE_TEST_IS_SYMLINK) || !g_file_test(app_dir, G_FILE_TEST_IS_DIR))
        return glnx_throw(error, "The application image has no /app directory");

    /* an application launched before a restart of the daemon may still use the
     * directory as a lower layer, so it is reused rather than removed: the layer
     * is rewritten in place, and overlayfs cleans the work directory up itself */
    if (!glnx_shutil_mkdir_p_at(AT_FDCWD, layer_dir, 0755, NULL, error) ||
        !glnx_shutil_mkdir_p_at(AT_FDCWD, work_dir, 0755, NULL, error) ||
        !glnx_shutil_mkdir_p_at(AT_FDCWD, tree->mount_path, 0755, NULL, error))
        return FALSE;
    if (!mfr_tree_prepare_layer(tree, layer_dir, error))
        return FALSE;
    if (!mfr_tree_run_ldconfig(tree, lower, layer_dir, work_dir, app_dir, error))
        return FALSE;
    if (!mfr_tree_mount_tree(tree, lower, layer_dir, app_dir, error))
        return FALSE;

    tree->executable = g_build_filename("/app/bin", tree->app->command, NULL);
    if (!mfr_tree_has_program(tree, tree->executable))
        return glnx_throw(error, "The application has no program %s", tree->executable);

    return TRUE;
}

/**
 * mfr_tree_assemble:
 * @app: the application to assemble a tree for
 * @captures: (element-type utf8 MfrCapture): the captures by the hash of their core
 * @img_mounts: the cache of mounted images
 *
 * Mount a tree for an application: its images are taken from the cache, the
 * capture of its core from the index, and the tree is assembled from these.
 *
 * Returns: (transfer full): the tree, or %NULL
 */
MfrTree *
mfr_tree_assemble(MfrApp *app, GHashTable *captures, MfrImageMounts *img_mounts, GError **error)
{
    g_autoptr(MfrTree) tree = g_object_new(MFR_TYPE_TREE, NULL);

    tree->app = g_object_ref(app);
    tree->img_mounts = g_object_ref(img_mounts);

    /* the capture is made against the mounted core, so the images come first */
    if (!mfr_tree_acquire_images(tree, error) || !mfr_tree_ensure_capture(tree, captures, error)) {
        mfr_tree_release_images(tree);
        return NULL;
    }

    tree->key = mfr_make_tree_key(app, tree->capture);
    tree->mount_path = g_build_filename(MFR_TREES_MOUNT_DIR, tree->key, NULL);
    tree->cache_dir = g_build_filename(MFR_TREES_CACHE_DIR, tree->key, NULL);
    tree->environment = mfr_build_environment_vars(tree->capture);

    g_info("Assembling tree %s for %s %s", tree->key, app->component_id, app->version);
    /* we keep the cache directory on failure, as an application launched before a
     * restart may still use it, see mfr_tree_remove_unused() */
    if (!mfr_tree_assemble_internal(tree, error)) {
        mfr_tree_teardown(tree);
        return NULL;
    }

    return g_steal_pointer(&tree);
}

/**
 * mfr_tree_matches:
 *
 * Whether a tree can be reused for an application: it is made of the images
 * the application consists of now, and its capture still matches the host.
 * A capture that was built anew has a new ID, so a tree made with the old
 * one does not match anymore either.
 */
gboolean
mfr_tree_matches(const MfrTree *tree, const MfrApp *app)
{
    g_autofree char *key = mfr_make_tree_key(app, tree->capture);

    return g_str_equal(tree->key, key) && mfr_capture_is_fresh(tree->capture);
}

/**
 * mfr_tree_clone:
 *
 * Clone the tree, to launch an application in the clone. The clone stays
 * intact when the tree is torn down.
 */
gboolean
mfr_tree_clone(const MfrTree *tree, int *ret_mount_fd, GError **error)
{
    int mount_fd = open_tree(AT_FDCWD, tree->mount_path, OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC | AT_RECURSIVE);
    if (mount_fd < 0)
        return glnx_throw_errno_prefix(error, "Unable to clone the tree at %s", tree->mount_path);

    *ret_mount_fd = mount_fd;
    return TRUE;
}

/**
 * mfr_collect_tree_layers:
 *
 * Add the tree layers among the lower directories of the overlays listed
 * in @mountinfo to @layers.
 */
static void
mfr_collect_tree_layers(const char *mountinfo, GHashTable *layers)
{
    g_auto(GStrv) lines = g_strsplit(mountinfo, "\n", -1);

    for (size_t i = 0; lines[i] != NULL; i++) {
        g_autofree char *super_options = mfr_mountinfo_get_super_options(lines[i], "overlay", NULL);
        g_auto(GStrv) options = NULL;

        if (super_options == NULL)
            continue;
        options = g_strsplit(super_options, ",", -1);
        for (size_t j = 0; options[j] != NULL; j++) {
            const char *value = NULL;
            g_auto(GStrv) dirs = NULL;

            /* one option for all lower directories, or one each with the new mount API */
            if (g_str_has_prefix(options[j], "lowerdir="))
                value = options[j] + strlen("lowerdir=");
            else if (g_str_has_prefix(options[j], "lowerdir+="))
                value = options[j] + strlen("lowerdir+=");
            if (value == NULL)
                continue;
            dirs = g_strsplit(value, ":", -1);
            for (size_t k = 0; dirs[k] != NULL; k++) {
                if (g_str_has_prefix(dirs[k], MFR_TREES_CACHE_DIR "/"))
                    g_hash_table_add(layers, g_strdup(dirs[k]));
            }
        }
    }
}

/**
 * mfr_tree_collect_used_layers:
 *
 * Collect the tree layers that a mount namespace on the system still has an overlay
 * of, as lower directory of an application launched before a restart. The
 * namespace of the daemon itself is left out, its trees are not in use by
 * anything that runs.
 *
 * Returns: (transfer full) (element-type utf8): the paths of the layers
 */
static GHashTable *
mfr_tree_collect_used_layers(void)
{
    g_autoptr(GHashTable) layers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_autoptr(GHashTable) seen_ns = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
    g_autoptr(GDir) procs = g_dir_open("/proc", 0, NULL);
    const char *name;
    struct stat st;
    gint64 ns;

    if (stat("/proc/self/ns/mnt", &st) == 0) {
        ns = (gint64) st.st_ino;
        g_hash_table_add(seen_ns, g_memdup2(&ns, sizeof(ns)));
    }
    while (procs != NULL && (name = g_dir_read_name(procs)) != NULL) {
        g_autofree char *ns_path = g_build_filename("/proc", name, "ns/mnt", NULL);
        g_autofree char *mountinfo_path = g_build_filename("/proc", name, "mountinfo", NULL);
        g_autofree char *mountinfo = NULL;

        if (!g_ascii_isdigit(name[0]) || stat(ns_path, &st) != 0)
            continue;
        /* one read per mount namespace, not per process */
        ns = (gint64) st.st_ino;
        if (g_hash_table_contains(seen_ns, &ns))
            continue;
        g_hash_table_add(seen_ns, g_memdup2(&ns, sizeof(ns)));
        if (!g_file_get_contents(mountinfo_path, &mountinfo, NULL, NULL))
            continue;
        mfr_collect_tree_layers(mountinfo, layers);
    }

    return g_steal_pointer(&layers);
}

/**
 * mfr_tree_collect_used_captures:
 *
 * Get a set of captures that trees still in use were made with.
 *
 * Returns: (transfer full) (element-type utf8): the IDs of the captures
 */
GHashTable *
mfr_tree_collect_used_captures(void)
{
    g_autoptr(GHashTable) layers = mfr_tree_collect_used_layers();
    g_autoptr(GHashTable) captures = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GHashTableIter iter;
    gpointer layer;

    g_hash_table_iter_init(&iter, layers);
    while (g_hash_table_iter_next(&iter, &layer, NULL)) {
        g_autofree char *record_path = g_build_filename(layer, ENVIRONMENT_RECORD, NULL);
        g_autoptr(sd_json_variant) record = mf_json_parse_file(record_path, NULL);
        const char *id = record != NULL ? mf_json_get_string(record, "capture") : NULL;

        if (id != NULL)
            g_hash_table_add(captures, g_strdup(id));
    }

    return g_steal_pointer(&captures);
}

/**
 * mfr_tree_remove_unused:
 *
 * Remove the directory of every tree that no mount uses anymore. Done when
 * the daemon starts, before it has assembled any tree of its own.
 */
void
mfr_tree_remove_unused(void)
{
    g_autoptr(GHashTable) used = mfr_tree_collect_used_layers();
    g_autoptr(GDir) entries = g_dir_open(MFR_TREES_CACHE_DIR, 0, NULL);
    const char *name;

    while (entries != NULL && (name = g_dir_read_name(entries)) != NULL) {
        g_autofree char *dir = g_build_filename(MFR_TREES_CACHE_DIR, name, NULL);
        g_autofree char *layer = g_build_filename(dir, LAYER_NAME, NULL);
        g_autoptr(GError) error = NULL;

        if (g_hash_table_contains(used, layer))
            continue;
        g_info("Removing tree %s: it is not in use", name);
        if (!glnx_shutil_rm_rf_at(AT_FDCWD, dir, NULL, &error))
            g_warning("%s", error->message);
    }
}

/**
 * mfr_tree_get_app:
 *
 * The application a tree is for.
 */
MfrApp *
mfr_tree_get_app(const MfrTree *tree)
{
    return tree->app;
}

/**
 * mfr_tree_get_key:
 *
 * The cache key of the tree.
 */
const char *
mfr_tree_get_key(const MfrTree *tree)
{
    return tree->key;
}

/**
 * mfr_tree_get_executable:
 *
 * The main executable to run for the application, as an absolute path.
 */
const char *
mfr_tree_get_executable(const MfrTree *tree)
{
    return tree->executable;
}

/**
 * mfr_tree_get_environment:
 *
 * The environment variables to set for the application.
 */
const char *const *
mfr_tree_get_environment(const MfrTree *tree)
{
    return (const char *const *) tree->environment;
}
