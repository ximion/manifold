/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mft-utils.h"

#include <fcntl.h>
#include <unistd.h>
#include <glib/gstdio.h>
#include <libglnx.h>

#include "mf-store-private.h"

/**
 * mft_fake_hash:
 *
 * Make up a hash that looks real, and is the same for the same seed.
 */
char *
mft_fake_hash(const char *seed)
{
    return g_compute_checksum_for_string(G_CHECKSUM_SHA256, seed, -1);
}

void
mft_make_dirs(const char *path)
{
    g_assert_cmpint(g_mkdir_with_parents(path, 0755), ==, 0);
}

void
mft_make_link(const char *target, const char *path)
{
    g_assert_cmpint(symlink(target, path), ==, 0);
}

void
mft_replace_link(const char *target, const char *path)
{
    g_assert_cmpint(g_unlink(path), ==, 0);
    mft_make_link(target, path);
}

void
mft_write_file(const char *path, const char *content)
{
    g_assert_true(g_file_set_contents(path, content, -1, NULL));
}

char *
mft_read_file(const char *path)
{
    char *content = NULL;

    g_assert_true(g_file_get_contents(path, &content, NULL, NULL));
    return content;
}

void
mft_replace_in_file(const char *path, const char *old, const char *new)
{
    g_autofree char *content = mft_read_file(path);
    g_auto(GStrv) parts = g_strsplit(content, old, -1);
    g_autofree char *replaced = g_strjoinv(new, parts);

    mft_write_file(path, replaced);
}

/**
 * mft_make_tmp_dir:
 *
 * Make a new temporary directory for the test of the given name.
 */
char *
mft_make_tmp_dir(const char *name)
{
    g_autofree char *template = g_strdup_printf("manifold-test-%s-XXXXXX", name);
    g_autoptr(GError) error = NULL;
    char *dir = g_dir_make_tmp(template, &error);

    g_assert_no_error(error);
    return dir;
}

void
mft_rm_rf(const char *path)
{
    g_autoptr(GError) error = NULL;

    g_assert_true(glnx_shutil_rm_rf_at(AT_FDCWD, path, NULL, &error));
    g_assert_no_error(error);
}

/**
 * mft_clear_store:
 *
 * Remove everything from the store of the sandbox.
 */
void
mft_clear_store(void)
{
    g_autoptr(GDir) dir = g_dir_open(MF_STORAGE_DIR, 0, NULL);
    const char *name;

    g_assert_nonnull(dir);
    while ((name = g_dir_read_name(dir)) != NULL) {
        g_autofree char *path = g_build_filename(MF_STORAGE_DIR, name, NULL);

        mft_rm_rf(path);
    }
}

char *
mft_metainfo_xml(const char *component_id)
{
    return g_strdup_printf("<component><id>%s</id></component>\n", component_id);
}

/**
 * mft_manifest_json:
 *
 * Write out a manifest as JSON.
 */
char *
mft_manifest_json(const MftTestManifest *m)
{
    g_autofree char *identity = mft_fake_hash(m->component_id);
    g_autofree char *base = NULL;
    g_autofree char *app = NULL;

    if (m->base_id != NULL) {
        g_autofree char *base_identity = mft_fake_hash(m->base_id);

        /* clang-format off */
        base = g_strdup_printf(
            ",\n"
            "   \"base\": {\n"
            "      \"component\": \"%s\",\n"
            "      \"version\": \"%s\",\n"
            "      \"revision\": %" G_GUINT64_FORMAT ",\n"
            "      \"identity\": \"%s\",\n"
            "      \"content-hash\": \"%s\"\n"
            "   }",
            m->base_id,
            m->base_version != NULL ? m->base_version : "1",
            m->base_revision != 0 ? m->base_revision : 1,
            base_identity,
            base_identity);
        /* clang-format on */
    }
    if (g_strcmp0(m->kind, "app") == 0 && !m->sdk)
        app = g_strdup(",\n   \"app\": {\n      \"command\": \"app\"\n   }");

    /* clang-format off */
    return g_strdup_printf(
        "{\n"
        "   \"format\": 1,\n"
        "   \"component\": \"%s\",\n"
        "   \"kind\": \"%s\",\n"
        "   \"version\": \"%s\",\n"
        "   \"revision\": %" G_GUINT64_FORMAT ",\n"
        "   \"architecture\": \"%s\",\n"
        "   \"sdk\": %s,\n"
        "   \"identity\": \"%s\",\n"
        "   \"content-hash\": \"%s\"%s%s,\n"
        "   \"image\": {\n"
        "      \"file\": \"%s\",\n"
        "      \"size\": %" G_GSIZE_FORMAT ",\n"
        "      \"sha256\": \"%s\",\n"
        "      \"blake3\": \"%s\",\n"
        "      \"verity-root-hash\": \"%s\",\n"
        "      \"data-offset\": %" G_GUINT64_FORMAT ",\n"
        "      \"hash-offset\": %" G_GUINT64_FORMAT "\n"
        "   }\n"
        "}",
        m->component_id,
        m->kind,
        m->version,
        m->revision != 0 ? m->revision : 1,
        m->architecture != NULL ? m->architecture : "x86_64",
        m->sdk ? "true" : "false",
        identity,
        m->content_hash != NULL ? m->content_hash : identity,
        base != NULL ? base : "",
        app != NULL ? app : "",
        m->image_file != NULL ? m->image_file : "i.raw",
        m->image_size != 0 ? m->image_size : 1,
        m->sha256 != NULL ? m->sha256 : identity,
        m->blake3 != NULL ? m->blake3 : identity,
        m->verity_root_hash != NULL ? m->verity_root_hash : identity,
        m->data_offset != 0 ? m->data_offset : 20480,
        m->hash_offset != 0 ? m->hash_offset : 22528);
    /* clang-format on */
}

/**
 * mft_write_version:
 * @root: the store, e.g. %MF_STORAGE_DIR
 * @subdir: where in the store the component goes, %MF_STORE_APPS_SUBDIR, %MF_STORE_PLUGINS_SUBDIR
 *   or %MF_STORE_RUNTIMES_SUBDIR
 * @manifest: the manifest of the component
 * @base_version_dir: the version directory of the base the base link names, %NULL for "current"
 * @mode: the install mode to record, or %NULL for none
 *
 * Write a version directory of a component, as the install daemon leaves
 * it: with its image, its base link and its manifest. The version written
 * last becomes the current one, whether it is the newest or not.
 */
void
mft_write_version(const char *root,
                  const char *subdir,
                  const MftTestManifest *manifest,
                  const char *base_version_dir,
                  const char *mode)
{
    g_autofree char *identity = mft_fake_hash(manifest->component_id);
    const char *hash = manifest->verity_root_hash != NULL ? manifest->verity_root_hash : identity;
    g_autofree char *component_dir = g_build_filename(root, subdir, manifest->component_id, NULL);
    g_autofree char *version_name = g_strdup_printf("%s-%" G_GUINT64_FORMAT,
                                                    manifest->version,
                                                    manifest->revision != 0 ? manifest->revision : 1);
    g_autofree char *version_dir = g_build_filename(component_dir, version_name, NULL);
    g_autofree char *image_link = g_build_filename(version_dir, mf_store_image_link_for_subdir(subdir), NULL);
    g_autofree char *image_target = g_strdup_printf("../../../" MF_STORE_IMAGES_SUBDIR "/%s.raw", hash);
    g_autofree char *image_name = g_strdup_printf("%s.raw", hash);
    g_autofree char *images_dir = g_build_filename(root, MF_STORE_IMAGES_SUBDIR, NULL);
    g_autofree char *image_path = g_build_filename(images_dir, image_name, NULL);
    g_autofree char *manifest_path = g_build_filename(version_dir, "manifest.json", NULL);
    g_autofree char *manifest_json = mft_manifest_json(manifest);
    g_autofree char *current_link = g_build_filename(component_dir, "current", NULL);

    mft_make_dirs(version_dir);
    mft_make_dirs(images_dir);
    mft_write_file(image_path, "not an image");
    mft_make_link(image_target, image_link);
    if (manifest->base_id != NULL) {
        g_autofree char *base_link = g_build_filename(version_dir, "base", NULL);
        g_autofree char *base_target = NULL;
        const char *base_subdir = MF_STORE_RUNTIMES_SUBDIR;

        /* we put an SDK on the component of its own kind, and everything else on a runtime */
        if (manifest->sdk)
            base_subdir = mf_store_subdir_for_kind(mf_component_kind_from_string(manifest->kind), FALSE);
        base_target = g_strdup_printf("../../../%s/%s/%s",
                                      base_subdir,
                                      manifest->base_id,
                                      base_version_dir != NULL ? base_version_dir : "current");
        mft_make_link(base_target, base_link);
    }
    mft_write_file(manifest_path, manifest_json);
    if (mode != NULL) {
        g_autofree char *mode_path = g_build_filename(version_dir, "mode", NULL);

        mft_write_file(mode_path, mode);
    }
    g_unlink(current_link);
    mft_make_link(version_name, current_link);
}
