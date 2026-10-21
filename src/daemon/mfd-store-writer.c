/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mfd-store-writer.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libglnx.h>

#include "mf-store-private.h"
#include "mf-utils-private.h"
#include "mfd-utils.h"

/**
 * MfdStoreWriter:
 *
 * Mdifies the local store of installed components.
 *
 * The writer adds and removes what the manager tells it to, without many checks.
 * Upon interruption, it leaves the directories in a state that the next cleanup
 * can tidy up.
 */
struct _MfdStoreWriter {
    GObject parent_instance;

    MfStore *store;
};

G_DEFINE_TYPE(MfdStoreWriter, mfd_store_writer, G_TYPE_OBJECT)

/* the subdirectories of the store that hold component directories */
static const char *const MFD_COMPONENT_SUBDIRS[] = {
    MF_STORE_APPS_SUBDIR,
    MF_STORE_PLUGINS_SUBDIR,
    MF_STORE_RUNTIMES_SUBDIR,
};

static void
mfd_store_writer_finalize(GObject *object)
{
    MfdStoreWriter *writer = MFD_STORE_WRITER(object);

    g_clear_object(&writer->store);

    G_OBJECT_CLASS(mfd_store_writer_parent_class)->finalize(object);
}

static void
mfd_store_writer_class_init(MfdStoreWriterClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = mfd_store_writer_finalize;
}

static void
mfd_store_writer_init(MfdStoreWriter *writer)
{
}

/**
 * mfd_store_writer_new:
 * @store: the store to write to
 *
 * Returns: (transfer full): a new #MfdStoreWriter
 */
MfdStoreWriter *
mfd_store_writer_new(MfStore *store)
{
    MfdStoreWriter *writer = g_object_new(MFD_TYPE_STORE_WRITER, NULL);

    writer->store = g_object_ref(store);
    return writer;
}

/**
 * mfd_store_writer_get_path:
 *
 * Returns: a path below the store
 */
static char *
mfd_store_writer_get_path(MfdStoreWriter *writer, const char *subdir, const char *name)
{
    return g_build_filename(mf_store_get_path(writer->store), subdir, name, NULL);
}

/**
 * mfd_store_writer_ensure_root:
 *
 * Create the directories of the store, if they do not exist.
 */
gboolean
mfd_store_writer_ensure_root(MfdStoreWriter *writer, GError **error)
{
    const char *const subdirs[] = {
        MF_STORE_IMAGES_SUBDIR,
        MF_STORE_APPS_SUBDIR,
        MF_STORE_PLUGINS_SUBDIR,
        MF_STORE_RUNTIMES_SUBDIR,
    };

    for (size_t i = 0; i < G_N_ELEMENTS(subdirs); i++) {
        g_autofree char *path = mfd_store_writer_get_path(writer, subdirs[i], NULL);

        if (!mfd_make_dir(path, error))
            return FALSE;
    }
    return TRUE;
}

/*
 * Images
 */

static char *
mfd_get_image_file_name(const char *verity_hash)
{
    return g_strconcat(verity_hash, MF_STORE_IMAGE_SUFFIX, NULL);
}

/**
 * mfd_store_writer_add_image:
 *
 * Copy an image of a bundle into the store, verifying it on the way. The
 * image is written without a name, and only gets its name after it was
 * verified completely: an interrupted copy leaves nothing behind.
 */
gboolean
mfd_store_writer_add_image(MfdStoreWriter *writer,
                           MfBundle *bundle,
                           MfBundleImage *image,
                           MfCopyProgressFunc progress_func,
                           gpointer user_data,
                           GError **error)
{
    g_auto(GLnxTmpfile) tmpf = {0};
    const char *verity_hash = mf_manifest_get_verity_hash(mf_bundle_image_get_manifest(image));
    g_autofree char *path = mf_store_image_path_at(mf_store_get_path(writer->store), verity_hash);
    g_autofree char *images_dir = mfd_store_writer_get_path(writer, MF_STORE_IMAGES_SUBDIR, NULL);

    if (!glnx_open_tmpfile_linkable_at(AT_FDCWD, images_dir, O_WRONLY | O_CLOEXEC, &tmpf, error))
        return FALSE;
    if (!mf_bundle_copy_image(bundle, image, tmpf.fd, progress_func, user_data, error))
        return FALSE;
    if (fchmod(tmpf.fd, 0644) < 0 || fsync(tmpf.fd) < 0) {
        glnx_throw_errno_prefix(error, "Unable to finish writing image %s", verity_hash);
        return FALSE;
    }

    /* the image may have appeared in the meantime; it is named by its hash, so it is the same */
    if (!glnx_link_tmpfile_at(&tmpf, GLNX_LINK_TMPFILE_NOREPLACE_IGNORE_EXIST, AT_FDCWD, path, error))
        return FALSE;

    mf_store_invalidate(writer->store);
    return TRUE;
}

/**
 * mfd_store_writer_remove_image:
 *
 * Remove an image. Whether something uses it is for the caller to check.
 */
gboolean
mfd_store_writer_remove_image(MfdStoreWriter *writer, const char *verity_hash, GError **error)
{
    g_autofree char *path = mf_store_image_path_at(mf_store_get_path(writer->store), verity_hash);

    g_info("Removing image %s", verity_hash);
    if (unlink(path) < 0 && errno != ENOENT) {
        glnx_throw_errno_prefix(error, "Unable to remove %s", path);
        return FALSE;
    }

    mf_store_invalidate(writer->store);
    return TRUE;
}

/*
 * Component directories
 */

/**
 * mfd_store_writer_update_metainfo_link:
 *
 * Make the MetaInfo of the current version of a component available in the
 * metadata of the "local" origin, with a link through the `current` link of
 * the component. We remove that link if the current version has no MetaInfo,
 * or if there is no current version.
 */
static gboolean
mfd_store_writer_update_metainfo_link(MfdStoreWriter *writer, const char *component_dir, GError **error)
{
    g_autofree char *component_id = g_path_get_basename(component_dir);
    g_autofree char *parent = g_path_get_dirname(component_dir);
    g_autofree char *dir_name = g_path_get_basename(parent);
    g_autofree char *file_name = g_strconcat(component_id, MF_METAINFO_SUFFIX, NULL);
    g_autofree char *current = g_build_filename(component_dir, MF_STORE_CURRENT_NAME, file_name, NULL);
    g_autofree char *metainfo_dir = mfd_store_writer_get_path(writer, MF_STORE_LOCAL_METAINFO_SUBDIR, NULL);
    g_autofree char *link_path = g_build_filename(metainfo_dir, file_name, NULL);
    g_autofree char *target = g_build_filename(MF_STORE_LOCAL_METAINFO_TO_ROOT,
                                               dir_name,
                                               component_id,
                                               MF_STORE_CURRENT_NAME,
                                               file_name,
                                               NULL);

    if (!g_file_test(current, G_FILE_TEST_IS_REGULAR))
        return mfd_remove_if_exists(link_path, error);
    if (!mfd_make_dir(metainfo_dir, error))
        return FALSE;
    return mfd_replace_link(target, metainfo_dir, file_name, error);
}

/**
 * mfd_store_writer_update_current:
 * @except: (nullable): the name of a version directory to leave out, as it is about to be removed
 *
 * Point the `current` link of a component at its newest installed
 * version, or remove it if there is none, and keep the MetaInfo link of
 * the local origin in step.
 */
static gboolean
mfd_store_writer_update_current(MfdStoreWriter *writer,
                                const char *component_dir,
                                const char *except,
                                GError **error)
{
    g_autofree char *link_path = g_build_filename(component_dir, MF_STORE_CURRENT_NAME, NULL);
    g_autofree char *newest_name = NULL;
    g_autofree char *newest_version = NULL;
    g_autoptr(GDir) dir = NULL;
    guint64 newest_revision = 0;
    const char *name;

    dir = g_dir_open(component_dir, 0, error);
    if (dir == NULL)
        return FALSE;
    while ((name = g_dir_read_name(dir)) != NULL) {
        g_autofree char *path = g_build_filename(component_dir, name, NULL);
        g_autofree char *version = NULL;
        guint64 revision;

        if (mfd_is_hidden(name) || g_strcmp0(name, except) == 0 || mfd_is_symlink(path) ||
            !mfd_is_directory(path))
            continue;
        if (!mf_split_full_version(name, &version, &revision))
            continue;

        if (newest_version == NULL ||
            mf_version_compare_with_rev(version, revision, newest_version, newest_revision) > 0) {
            g_free(newest_name);
            g_free(newest_version);
            newest_name = g_strdup(name);
            newest_version = g_steal_pointer(&version);
            newest_revision = revision;
        }
    }

    if (newest_name == NULL) {
        if (!mfd_remove_if_exists(link_path, error))
            return FALSE;
    } else if (!mfd_replace_link(newest_name, component_dir, MF_STORE_CURRENT_NAME, error)) {
        return FALSE;
    }

    return mfd_store_writer_update_metainfo_link(writer, component_dir, error);
}

/**
 * mfd_store_writer_install_component:
 * @writer: the #MfdStoreWriter
 * @manifest: the manifest of the component, as it was in the bundle
 * @base: (nullable): what the component builds on, or %NULL for a core
 * @metainfo: the MetaInfo of the component, sanitized
 * @mode: whether the component is a potential target for automatic removal
 *
 * Record an installed version of a component: a directory with links to its
 * image and its base, its manifest and MetaInfo, where it came from, and its
 * install mode. We write the directory under a temporary name, and rename it
 * once it is complete, so it appears as a whole.
 *
 * Then we point the `current` link of the component at its newest version.
 * The version is installed by then, so if that fails, we only warn about it.
 *
 * A version that is installed already stays as it is, except that we mark it
 * as "manual" if @mode is "manual" now.
 *
 * This method assumes all checks and validation have already happened on the caller's side!
 * It just executes the write operation.
 */
gboolean
mfd_store_writer_install_component(MfdStoreWriter *writer,
                                   MfManifest *manifest,
                                   const MfBaseLink *base,
                                   GBytes *metainfo,
                                   MfInstallMode mode,
                                   GError **error)
{
    const char *subdir = mf_store_subdir_for_kind(mf_manifest_get_kind(manifest),
                                                  mf_manifest_is_sdk(manifest));
    const char *component_id = mf_manifest_get_component_id(manifest);
    const char *version = mf_manifest_get_version(manifest);
    guint64 revision = mf_manifest_get_revision(manifest);
    g_autofree char *component_dir = mfd_store_writer_get_path(writer, subdir, component_id);
    g_autofree char *metainfo_name = g_strconcat(component_id, MF_METAINFO_SUFFIX, NULL);
    g_autofree char *version_name = mf_build_full_version(version, revision);
    g_autofree char *tmp_name = g_strconcat(MF_TMP_PREFIX, version_name, NULL);
    g_autofree char *tmp_dir = g_build_filename(component_dir, tmp_name, NULL);
    g_autofree char *final_dir = g_build_filename(component_dir, version_name, NULL);
    g_autoptr(GError) cleanup_error = NULL;
    g_autoptr(GError) current_error = NULL;
    gconstpointer manifest_data;
    gsize manifest_size;
    gboolean ret = FALSE;

    if (mfd_is_directory(final_dir)) {
        if (mode != MF_INSTALL_MODE_MANUAL)
            return TRUE;
        if (!mfd_write_word(final_dir, MF_STORE_MODE_NAME, mf_install_mode_to_string(mode), error))
            return FALSE;
        mf_store_invalidate(writer->store);
        return TRUE;
    }

    if (!mfd_make_dir(component_dir, error))
        return FALSE;
    if (!mfd_remove_if_exists(tmp_dir, error))
        return FALSE;
    if (mkdir(tmp_dir, 0755) < 0) {
        glnx_throw_errno_prefix(error, "Unable to create directory %s", tmp_dir);
        return FALSE;
    }

    {
        g_autofree char *file_name = mfd_get_image_file_name(mf_manifest_get_verity_hash(manifest));
        g_autofree char *target = g_build_filename(MF_STORE_COMPONENT_TO_IMAGES, file_name, NULL);
        g_autofree char *path = g_build_filename(tmp_dir, mf_store_image_link_for_subdir(subdir), NULL);

        if (!mfd_make_symlink(target, path, error))
            goto out;
    }
    if (base != NULL) {
        g_autofree char *target = mf_base_link_get_target(base);
        g_autofree char *path = g_build_filename(tmp_dir, MF_STORE_BASE_LINK, NULL);

        if (!mfd_make_symlink(target, path, error))
            goto out;
    }
    manifest_data = g_bytes_get_data(mf_manifest_get_data(manifest), &manifest_size);
    if (!mfd_write_marker(tmp_dir, MF_STORE_MANIFEST_NAME, manifest_data, (gssize) manifest_size, error))
        goto out;
    if (!mfd_write_marker(tmp_dir,
                          metainfo_name,
                          g_bytes_get_data(metainfo, NULL),
                          (gssize) g_bytes_get_size(metainfo),
                          error))
        goto out;

    /* TODO: every installation is local until repository support is ported from Intrafold */
    if (!mfd_write_word(tmp_dir, MF_STORE_ORIGIN_NAME, MF_STORE_LOCAL_ORIGIN, error))
        goto out;
    if (!mfd_write_word(tmp_dir, MF_STORE_MODE_NAME, mf_install_mode_to_string(mode), error))
        goto out;

    if (rename(tmp_dir, final_dir) < 0) {
        if (errno == EEXIST || errno == ENOTEMPTY)
            g_set_error(error,
                        MF_ERROR,
                        MF_ERROR_ALREADY_INSTALLED,
                        "%s %s is installed already",
                        component_id,
                        version);
        else
            glnx_throw_errno_prefix(error, "Unable to rename %s", tmp_dir);
        goto out;
    }

    g_info("Installed %s %s%s",
           component_id,
           version,
           mode == MF_INSTALL_MODE_AUTO ? " as a dependency" : "");
    ret = TRUE;
    if (!mfd_store_writer_update_current(writer, component_dir, NULL, &current_error))
        g_warning("Unable to make the newest version of %s current: %s",
                  component_id,
                  current_error->message);

    /* invalidate the cached data */
    mf_store_invalidate(writer->store);

out:
    if (!ret && !mfd_remove_if_exists(tmp_dir, &cleanup_error))
        g_warning("%s", cleanup_error->message);

    return ret;
}

/**
 * mfd_store_writer_remove_version_dir:
 *
 * Remove a version directory, and the directory of the component if that
 * was its last version. Before the version goes, `current` of the component
 * is pointed at the newest other version, so it never points at nothing.
 *
 * We remove the version from the store by renaming its directory to a hidden
 * name, and then delete that. If deleting fails, we only warn about it, and
 * the next cleanup deletes it.
 */
gboolean
mfd_store_writer_remove_version_dir(MfdStoreWriter *writer, const char *version_dir, GError **error)
{
    g_autofree char *component_dir = g_path_get_dirname(version_dir);
    g_autofree char *version_name = g_path_get_basename(version_dir);
    g_autofree char *tmp_name = g_strconcat(MF_TMP_PREFIX, version_name, NULL);
    g_autofree char *tmp_dir = g_build_filename(component_dir, tmp_name, NULL);
    g_autoptr(GError) local_error = NULL;

    g_info("Removing %s", version_dir);
    if (!mfd_remove_if_exists(tmp_dir, error))
        return FALSE;

    mf_store_invalidate(writer->store);
    if (!mfd_store_writer_update_current(writer, component_dir, version_name, error))
        goto undo;
    /* we rename it first, so an interrupted removal only leaves a hidden directory behind */
    if (rename(version_dir, tmp_dir) < 0) {
        glnx_throw_errno_prefix(error, "Unable to rename %s", version_dir);
        goto undo;
    }

    if (!mfd_remove_if_exists(tmp_dir, &local_error))
        g_warning("Unable to delete the removed %s, cleaning up does: %s", version_dir, local_error->message);
    if (rmdir(component_dir) < 0 && errno != ENOTEMPTY && errno != EEXIST && errno != ENOENT)
        return glnx_throw_errno_prefix(error, "Unable to remove directory %s", component_dir);
    return TRUE;

undo:
    if (!mfd_store_writer_update_current(writer, component_dir, NULL, &local_error))
        g_warning("Unable to make %s current again: %s", version_dir, local_error->message);
    return FALSE;
}

/**
 * mfd_store_writer_set_mode:
 * @writer: the #MfdStoreWriter
 * @version_dir: the version directory of a component
 * @mode: the install mode to record
 *
 * Record whether a version is kept until it is removed explicitly, or
 * removed once it is not needed anymore.
 */
gboolean
mfd_store_writer_set_mode(MfdStoreWriter *writer, const char *version_dir, MfInstallMode mode, GError **error)
{
    if (!mfd_write_word(version_dir, MF_STORE_MODE_NAME, mf_install_mode_to_string(mode), error))
        return FALSE;
    mf_store_invalidate(writer->store);
    return TRUE;
}

/**
 * mfd_store_writer_remove_cruft:
 *
 * Remove what interrupted installations and removals left in the component
 * directories.
 */
gboolean
mfd_store_writer_remove_cruft(MfdStoreWriter *writer, GError **error)
{
    for (size_t i = 0; i < G_N_ELEMENTS(MFD_COMPONENT_SUBDIRS); i++) {
        const char *name;
        g_autofree char *subdir = mfd_store_writer_get_path(writer, MFD_COMPONENT_SUBDIRS[i], NULL);
        g_autoptr(GDir) components = g_dir_open(subdir, 0, NULL);

        while (components != NULL && (name = g_dir_read_name(components)) != NULL) {
            g_autofree char *component_dir = g_build_filename(subdir, name, NULL);
            g_autoptr(GDir) entries = NULL;
            const char *entry;

            if (mfd_is_hidden(name) || !mfd_is_directory(component_dir))
                continue;
            entries = g_dir_open(component_dir, 0, NULL);
            while (entries != NULL && (entry = g_dir_read_name(entries)) != NULL) {
                g_autofree char *path = g_build_filename(component_dir, entry, NULL);

                if (g_str_has_prefix(entry, MF_TMP_PREFIX) && !mfd_remove_if_exists(path, error))
                    return FALSE;
            }
        }
    }

    return TRUE;
}
