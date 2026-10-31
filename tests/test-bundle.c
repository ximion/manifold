/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <glib/gstdio.h>
#include <blake3.h>
#include <zip.h>
#include <manifold.h>

#include "mft-utils.h"

/* an image of a bundle, as the test makes it up */
typedef struct TestImage TestImage;
struct TestImage {
    const char *component_id;
    const char *kind;
    const char *version;
    guint64 revision;
    const TestImage *base; /* NULL for a core, or for a broken manifest */
    GBytes *data;
};

/* what can be wrong with a bundle */
typedef struct {
    gboolean unsigned_;
    const char *unlisted_file; /* a file the index does not list */
    const TestImage *corrupt;  /* an image whose content differs from its manifest */
    const TestImage *tampered; /* an image whose manifest differs from the index */
    const TestImage *missing;  /* an image that is not in the bundle */
    /* an image whose manifest records a wrong blake3, next to the right sha256 */
    const TestImage *wrong_blake3;
    /* an image whose manifest the index lists twice */
    const TestImage *listed_twice;
    /* an image the catalog has no component for */
    const TestImage *undescribed;
    /* an image whose component in the catalog has another ID */
    const TestImage *misdescribed;
    /* the icon of the app is not a JPEG XL image */
    gboolean bad_icon;
    /* the screenshot of the app differs from what the index records */
    gboolean corrupt_media;
} BundleFlaws;

/* the start of a JPEG XL codestream */
static const guint8 JXL_DATA[] = {0xff, 0x0a, 'f', 'a', 'k', 'e'};

typedef struct {
    char *dir;
    TestImage core;
    TestImage kit;
    TestImage app;
} Fixture;

static char *
mft_blake3_of(gconstpointer data, gsize size)
{
    GString *hex = g_string_new(NULL);
    guint8 out[BLAKE3_OUT_LEN];
    blake3_hasher hasher;

    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, data, size);
    blake3_hasher_finalize(&hasher, out, sizeof(out));
    for (size_t i = 0; i < sizeof(out); i++)
        g_string_append_printf(hex, "%02x", out[i]);
    return g_string_free(hex, FALSE);
}

static char *
mft_blake3_of_bytes(GBytes *bytes)
{
    gsize size;
    gconstpointer data = g_bytes_get_data(bytes, &size);

    return mft_blake3_of(data, size);
}

static GBytes *
mft_fake_image_data(const char *seed, gsize size)
{
    g_autofree guint8 *data = g_malloc(size);

    for (gsize i = 0; i < size; i++)
        data[i] = (guint8) (seed[i % strlen(seed)] + i);
    return g_bytes_new_take(g_steal_pointer(&data), size);
}

static char *
mft_image_file_name(const TestImage *image)
{
    return g_strdup_printf("%s_%s.raw", image->component_id, image->version);
}

static char *
mft_image_manifest_json(const TestImage *image, gboolean wrong_blake3)
{
    g_autofree char *file_name = mft_image_file_name(image);
    g_autofree char *content_hash = mft_fake_hash(file_name);
    g_autofree char *sha256 = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, image->data);
    g_autofree char *blake3 = wrong_blake3 ? mft_fake_hash("wrong") : mft_blake3_of_bytes(image->data);
    g_autofree char *verity_hash = mft_fake_hash(sha256);
    const TestImage *base = image->base;

    return mft_manifest_json(&(MftTestManifest){
        .component_id = image->component_id,
        .kind = image->kind,
        .version = image->version,
        .revision = image->revision,
        .content_hash = content_hash,
        .base_id = base != NULL ? base->component_id : NULL,
        .base_version = base != NULL ? base->version : NULL,
        .base_revision = base != NULL ? base->revision : 0,
        .image_file = file_name,
        .image_size = g_bytes_get_size(image->data),
        .sha256 = sha256,
        .blake3 = blake3,
        .verity_root_hash = verity_hash,
        .hash_offset = 24576,
    });
}

static void
mft_bundle_add(zip_t *zip, const char *name, gconstpointer data, gsize size)
{
    zip_source_t *source = zip_source_buffer(zip, g_memdup2(data, size), size, 1);

    g_assert_nonnull(source);
    g_assert_cmpint(zip_file_add(zip, name, source, ZIP_FL_ENC_UTF_8), >=, 0);
}

static void
mft_index_add(GString *index, const char *name, gconstpointer data, gsize size)
{
    g_autofree char *sha256 = g_compute_checksum_for_data(G_CHECKSUM_SHA256, data, size);
    g_autofree char *blake3 = mft_blake3_of(data, size);

    /* clang-format off */
    g_string_append_printf(
        index,
        "%s\n"
        "      {\n"
        "         \"name\": \"%s\",\n"
        "         \"size\": %zu,\n"
        "         \"sha256\": \"%s\",\n"
        "         \"blake3\": \"%s\"\n"
        "      }",
        index->str[index->len - 1] == '[' ? "" : ",",
        name,
        size,
        sha256,
        blake3);
    /* clang-format on */
}

/**
 * mft_add_file:
 *
 * Add a file to a bundle and to its index.
 */
static void
mft_add_file(zip_t *zip, GString *index, const char *name, gconstpointer data, gsize size)
{
    mft_index_add(index, name, data, size);
    mft_bundle_add(zip, name, data, size);
}

/**
 * mft_write_catalog:
 *
 * Add the catalog of the images to a bundle, with an icon and a screenshot
 * for the app, and the given flaws.
 */
static void
mft_write_catalog(zip_t *zip, GString *index, const TestImage *const *images, const BundleFlaws *flaws)
{
    g_autoptr(GString) components = g_string_new(NULL);
    g_autofree char *catalog = NULL;

    for (size_t i = 0; images[i] != NULL; i++) {
        const TestImage *image = images[i];
        g_autofree char *ref = g_strdup_printf("x86_64/%s/%s", image->component_id, image->version);
        g_autofree char *extra = NULL;
        g_autofree char *component = NULL;

        if (image == flaws->undescribed)
            continue;
        if (g_str_equal(image->kind, "app")) {
            g_autofree char *icon_name = g_strdup_printf("catalog/icons/64x64/%s.jxl", image->component_id);
            g_autofree char *media_name = g_strdup_printf("catalog/media/%s/screenshot.jxl",
                                                          image->component_id);

            extra = g_strdup_printf(
                "<icon type=\"cached\" width=\"64\" height=\"64\">%s.jxl</icon>"
                "<screenshots><screenshot type=\"default\">"
                "<image type=\"source\">%s/screenshot.jxl</image>"
                "</screenshot></screenshots>",
                image->component_id,
                image->component_id);
            if (flaws->bad_icon)
                mft_add_file(zip, index, icon_name, "not an image", strlen("not an image"));
            else
                mft_add_file(zip, index, icon_name, JXL_DATA, sizeof(JXL_DATA));

            mft_index_add(index, media_name, JXL_DATA, sizeof(JXL_DATA));
            if (flaws->corrupt_media)
                mft_bundle_add(zip, media_name, "changed", strlen("changed"));
            else
                mft_bundle_add(zip, media_name, JXL_DATA, sizeof(JXL_DATA));
        }
        component = mft_catalog_component_xml(image == flaws->misdescribed ? "org.example.Other"
                                                                           : image->component_id,
                                              ref,
                                              extra);
        g_string_append(components, component);
    }

    catalog = mft_catalog_xml(components->str);
    mft_add_file(zip, index, "catalog/manifold-local.xml", catalog, strlen(catalog));
}

/**
 * mft_write_bundle:
 *
 * Write a bundle of the images, with the given flaws, and return its path.
 */
static char *
mft_write_bundle(Fixture *f, const char *name, const TestImage *const *images, const BundleFlaws *flaws)
{
    g_autofree char *path = g_build_filename(f->dir, name, NULL);
    g_autoptr(GString) index = g_string_new("{\"format\": 1,\n\"files\": [");
    zip_t *zip = zip_open(path, ZIP_CREATE | ZIP_TRUNCATE, NULL);

    g_assert_nonnull(zip);
    for (size_t i = 0; images[i] != NULL; i++) {
        const TestImage *image = images[i];
        g_autofree char *file_name = mft_image_file_name(image);
        g_autofree char *base_name = g_strndup(file_name, strlen(file_name) - strlen(".raw"));
        g_autofree char *manifest_name = g_strconcat(base_name, ".manifold.json", NULL);
        g_autofree char *manifest = mft_image_manifest_json(image, image == flaws->wrong_blake3);
        gsize data_size;
        const guint8 *data = g_bytes_get_data(image->data, &data_size);

        mft_index_add(index, manifest_name, manifest, strlen(manifest));
        if (image == flaws->listed_twice)
            mft_index_add(index, manifest_name, manifest, strlen(manifest));

        if (image == flaws->tampered) {
            g_autofree char *changed = g_strconcat(manifest, "\n", NULL);

            mft_bundle_add(zip, manifest_name, changed, strlen(changed));
        } else {
            mft_bundle_add(zip, manifest_name, manifest, strlen(manifest));
        }

        if (image == flaws->corrupt) {
            g_autofree guint8 *changed = g_memdup2(data, data_size);

            changed[0] ^= 0xff;
            mft_bundle_add(zip, file_name, changed, data_size);
        } else if (image != flaws->missing) {
            mft_bundle_add(zip, file_name, data, data_size);
        }
    }
    mft_write_catalog(zip, index, images, flaws);
    g_string_append(index, "\n   ]\n}");
    mft_bundle_add(zip, "index.json", index->str, index->len);

    if (!flaws->unsigned_)
        mft_bundle_add(zip, "index.json.sig", "not a signature", strlen("not a signature"));
    if (flaws->unlisted_file != NULL)
        mft_bundle_add(zip, flaws->unlisted_file, "rm -rf /", strlen("rm -rf /"));

    g_assert_cmpint(zip_close(zip), ==, 0);
    return g_steal_pointer(&path);
}

static MfBundle *
mft_open_bundle(const char *path, GError **error)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);

    g_assert_cmpint(fd, >=, 0);
    auto bundle = mf_bundle_new_from_fd(fd, error);
    close(fd);
    return bundle;
}

static MfBundleImage *
mft_find_image(MfBundle *bundle, const char *component_id)
{
    GPtrArray *images = mf_bundle_get_images(bundle);

    for (guint i = 0; i < images->len; i++) {
        MfBundleImage *image = g_ptr_array_index(images, i);

        if (g_strcmp0(mf_manifest_get_component_id(mf_bundle_image_get_manifest(image)), component_id) == 0)
            return image;
    }
    g_assert_not_reached();
}

static MfManifest *
mft_find_manifest(MfBundle *bundle, const char *component_id)
{
    return mf_bundle_image_get_manifest(mft_find_image(bundle, component_id));
}

static void
mft_setup(Fixture *f, gconstpointer user_data)
{
    f->dir = mft_make_tmp_dir("bundle");

    f->core = (TestImage){TEST_CORE_ID, "core", "13.4", 22910000, NULL, mft_fake_image_data("core", 8192)};
    f->kit = (TestImage){TEST_KIT_ID,
                         "kit",
                         "20260112T0238",
                         22910051,
                         &f->core,
                         mft_fake_image_data("kit", 4096)};
    f->app = (TestImage){TEST_APP_ID, "app", "0.1.0", 22931700, &f->kit, mft_fake_image_data("app", 12288)};
}

static void
mft_teardown(Fixture *f, gconstpointer user_data)
{
    mft_rm_rf(f->dir);
    g_free(f->dir);
    g_bytes_unref(f->core.data);
    g_bytes_unref(f->kit.data);
    g_bytes_unref(f->app.data);
}

/* what a copy reported as its progress */
typedef struct {
    guint64 bytes_copied;
    guint n_reports;
} CopyProgress;

static void
mft_count_progress(guint64 bytes_copied, gpointer user_data)
{
    CopyProgress *progress = user_data;

    g_assert_cmpuint(bytes_copied, >, progress->bytes_copied);
    progress->bytes_copied = bytes_copied;
    progress->n_reports++;
}

/*
 * Tests
 */

/**
 * test_valid_bundle:
 */
static void
test_valid_bundle(Fixture *f, gconstpointer user_data)
{
    g_autofree char *path = mft_write_bundle(f,
                                             "app.manifold",
                                             (const TestImage *[]){&f->kit, &f->app, NULL},
                                             &(BundleFlaws){});
    g_autoptr(GError) error = NULL;
    g_autoptr(MfBundle) bundle = mft_open_bundle(path, &error);
    g_autofree char *expected_sha256 = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, f->app.data);
    g_autofree char *expected_blake3 = mft_blake3_of_bytes(f->app.data);
    g_autofree char *expected_verity = mft_fake_hash(expected_sha256);
    g_autofree char *kit_identity = mft_fake_hash(TEST_KIT_ID);
    GHashTable *icons;
    GBytes *icon;

    g_assert_no_error(error);
    g_assert_true(mf_bundle_is_signed(bundle));
    g_assert_cmpuint(mf_bundle_get_images(bundle)->len, ==, 2);

    MfBundleImage *app = mft_find_image(bundle, TEST_APP_ID);
    MfManifest *manifest = mf_bundle_image_get_manifest(app);
    g_assert_cmpint(mf_manifest_get_kind(manifest), ==, MF_COMPONENT_KIND_APP);
    g_assert_cmpstr(mf_manifest_get_version(manifest), ==, "0.1.0");
    g_assert_cmpuint(mf_manifest_get_revision(manifest), ==, 22931700);
    g_assert_cmpstr(mf_manifest_get_architecture(manifest), ==, "x86_64");
    g_assert_cmpstr(mf_manifest_get_command(manifest), ==, "app");
    g_assert_cmpstr(mf_bundle_image_get_manifest_name(app), ==, TEST_APP_ID "_0.1.0.manifold.json");
    g_assert_cmpstr(mf_manifest_get_image_file(manifest), ==, TEST_APP_ID "_0.1.0.raw");
    g_assert_cmpuint(mf_manifest_get_image_size(manifest), ==, 12288);
    g_assert_cmpstr(mf_manifest_get_image_checksum(manifest, MF_CHECKSUM_KIND_SHA256), ==, expected_sha256);
    g_assert_cmpstr(mf_manifest_get_image_checksum(manifest, MF_CHECKSUM_KIND_BLAKE3), ==, expected_blake3);
    g_assert_cmpstr(mf_manifest_get_verity_hash(manifest), ==, expected_verity);
    g_assert_cmpuint(mf_manifest_get_data_offset(manifest), ==, 20480);
    g_assert_cmpuint(mf_manifest_get_hash_offset(manifest), ==, 24576);
    g_assert_cmpstr(mf_manifest_get_base_id(manifest), ==, TEST_KIT_ID);
    g_assert_cmpstr(mf_manifest_get_base_version(manifest), ==, "20260112T0238");
    g_assert_cmpuint(mf_manifest_get_base_revision(manifest), ==, 22910051);
    g_assert_cmpstr(mf_manifest_get_base_identity(manifest), ==, kit_identity);
    g_assert_cmpstr(mf_manifest_get_base_content_hash(manifest), ==, kit_identity);
    g_assert_false(mf_manifest_is_sdk(manifest));
    g_assert_cmpstr(mf_manifest_get_base_id(mft_find_manifest(bundle, TEST_KIT_ID)), ==, TEST_CORE_ID);

    /* the app's base is found in the bundle by its identity */
    g_assert_true(mf_bundle_find_image_for_identity(bundle, kit_identity) ==
                  mft_find_image(bundle, TEST_KIT_ID));
    g_assert_null(mf_bundle_find_image_for_identity(bundle, "no-such-identity"));

    /* every image has its own catalog data, the app also its icon */
    g_assert_nonnull(
        strstr(g_bytes_get_data(mf_bundle_image_get_catalog(app), NULL), "<id>" TEST_APP_ID "</id>"));
    g_assert_nonnull(
        strstr(g_bytes_get_data(mf_bundle_image_get_catalog(app), NULL), "origin=\"manifold-local\""));
    g_assert_null(strstr(g_bytes_get_data(mf_bundle_image_get_catalog(app), NULL), TEST_KIT_ID));
    g_assert_nonnull(
        strstr(g_bytes_get_data(mf_bundle_image_get_catalog(mft_find_image(bundle, TEST_KIT_ID)), NULL),
               "<id>" TEST_KIT_ID "</id>"));
    icons = mf_bundle_image_get_icons(app);
    g_assert_cmpuint(g_hash_table_size(icons), ==, 1);
    icon = g_hash_table_lookup(icons, "64x64/" TEST_APP_ID ".jxl");
    g_assert_nonnull(icon);
    g_assert_cmpmem(g_bytes_get_data(icon, NULL), g_bytes_get_size(icon), JXL_DATA, sizeof(JXL_DATA));
    g_assert_cmpuint(g_hash_table_size(mf_bundle_image_get_icons(mft_find_image(bundle, TEST_KIT_ID))),
                     ==,
                     0);
}

/**
 * test_core_has_no_base:
 */
static void
test_core_has_no_base(Fixture *f, gconstpointer user_data)
{
    g_autofree char *path = mft_write_bundle(f,
                                             "core.manifold",
                                             (const TestImage *[]){&f->core, NULL},
                                             &(BundleFlaws){.unsigned_ = TRUE});
    g_autoptr(GError) error = NULL;
    g_autoptr(MfBundle) bundle = mft_open_bundle(path, &error);
    MfManifest *core;

    g_assert_no_error(error);
    /* a bundle without a signature can be opened, it is just not signed */
    g_assert_false(mf_bundle_is_signed(bundle));
    core = mft_find_manifest(bundle, TEST_CORE_ID);
    g_assert_cmpint(mf_manifest_get_kind(core), ==, MF_COMPONENT_KIND_CORE);
    g_assert_null(mf_manifest_get_base_id(core));
    g_assert_null(mf_manifest_get_base_identity(core));
}

/**
 * test_copy_image:
 */
static void
test_copy_image(Fixture *f, gconstpointer user_data)
{
    /* mf_bundle_copy_image() copies 1 MiB at a time: this image takes three pieces, the last of which is not full */
    gsize image_size = 2 * 1024 * 1024 + 12288;
    g_autofree char *path = NULL;
    g_autofree char *copy_path = g_build_filename(f->dir, "copy.raw", NULL);
    g_autoptr(GError) error = NULL;
    g_autoptr(MfBundle) bundle = NULL;
    g_autofree char *copied = NULL;
    gsize copied_size;
    CopyProgress progress = {};
    int fd;

    g_bytes_unref(f->app.data);
    f->app.data = mft_fake_image_data("app", image_size);
    path = mft_write_bundle(f,
                            "app.manifold",
                            (const TestImage *[]){&f->kit, &f->app, NULL},
                            &(BundleFlaws){});
    bundle = mft_open_bundle(path, &error);
    g_assert_no_error(error);

    fd = open(copy_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    g_assert_cmpint(fd, >=, 0);
    g_assert_true(mf_bundle_copy_image(bundle,
                                       mft_find_image(bundle, TEST_APP_ID),
                                       fd,
                                       mft_count_progress,
                                       &progress,
                                       &error));
    g_assert_no_error(error);
    close(fd);

    /* the progress is reported after every piece */
    g_assert_cmpuint(progress.n_reports, ==, 3);
    g_assert_cmpuint(progress.bytes_copied, ==, image_size);
    g_assert_true(g_file_get_contents(copy_path, &copied, &copied_size, NULL));
    g_assert_cmpmem(copied, copied_size, g_bytes_get_data(f->app.data, NULL), image_size);
}

/**
 * test_corrupt_image:
 */
static void
test_corrupt_image(Fixture *f, gconstpointer user_data)
{
    g_autofree char *path = mft_write_bundle(f,
                                             "corrupt.manifold",
                                             (const TestImage *[]){&f->kit, &f->app, NULL},
                                             &(BundleFlaws){.corrupt = &f->app});
    g_autoptr(GError) error = NULL;
    g_autoptr(MfBundle) bundle = mft_open_bundle(path, &error);
    int fd = open("/dev/null", O_WRONLY | O_CLOEXEC);

    /* opening the bundle does not verify its images, copying one out does */
    g_assert_no_error(error);
    g_assert_false(mf_bundle_copy_image(bundle, mft_find_image(bundle, TEST_APP_ID), fd, NULL, NULL, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_VERIFICATION_FAILED);
    g_clear_error(&error);
    close(fd);

    /* and so does verifying one in place */
    g_assert_false(mf_bundle_verify_image(bundle, mft_find_image(bundle, TEST_APP_ID), NULL, NULL, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_VERIFICATION_FAILED);
    g_clear_error(&error);
    g_assert_true(mf_bundle_verify_image(bundle, mft_find_image(bundle, TEST_KIT_ID), NULL, NULL, &error));
    g_assert_no_error(error);
}

/**
 * test_missing_image:
 */
static void
test_missing_image(Fixture *f, gconstpointer user_data)
{
    g_autofree char *path = mft_write_bundle(f,
                                             "missing.manifold",
                                             (const TestImage *[]){&f->kit, &f->app, NULL},
                                             &(BundleFlaws){.missing = &f->kit});
    g_autoptr(GError) error = NULL;
    g_autoptr(MfBundle) bundle = mft_open_bundle(path, &error);

    g_assert_null(bundle);
    g_assert_error(error, MF_ERROR, MF_ERROR_VERIFICATION_FAILED);
    g_assert_cmpstr(error->message,
                    ==,
                    TEST_KIT_ID "_20260112T0238.manifold.json refers to " TEST_KIT_ID
                                "_20260112T0238.raw, which is not in the bundle");
}

/**
 * test_listed_twice:
 *
 * Every file of a bundle is referred to once, so an index can not list a
 * file twice.
 */
static void
test_listed_twice(Fixture *f, gconstpointer user_data)
{
    g_autofree char *path = mft_write_bundle(f,
                                             "twice.manifold",
                                             (const TestImage *[]){&f->kit, &f->app, NULL},
                                             &(BundleFlaws){.listed_twice = &f->app});
    g_autoptr(GError) error = NULL;
    g_autoptr(MfBundle) bundle = mft_open_bundle(path, &error);

    g_assert_null(bundle);
    g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_BUNDLE);
    g_assert_cmpstr(error->message,
                    ==,
                    "index.json refers to " TEST_APP_ID "_0.1.0.manifold.json, which is referred to already");
}

/**
 * test_tampered_manifest:
 */
static void
test_tampered_manifest(Fixture *f, gconstpointer user_data)
{
    g_autofree char *path = mft_write_bundle(f,
                                             "tampered.manifold",
                                             (const TestImage *[]){&f->kit, &f->app, NULL},
                                             &(BundleFlaws){.tampered = &f->app});
    g_autoptr(GError) error = NULL;
    g_autoptr(MfBundle) bundle = mft_open_bundle(path, &error);

    g_assert_null(bundle);
    g_assert_error(error, MF_ERROR, MF_ERROR_VERIFICATION_FAILED);
    g_assert_cmpstr(error->message,
                    ==,
                    TEST_APP_ID "_0.1.0.manifold.json does not match what the index records");
}

/**
 * test_wrong_blake3:
 *
 * Both hashes of an image are checked: a wrong blake3 fails the image,
 * however right its sha256 is
 */
static void
test_wrong_blake3(Fixture *f, gconstpointer user_data)
{
    g_autofree char *path = mft_write_bundle(f,
                                             "blake3.manifold",
                                             (const TestImage *[]){&f->kit, &f->app, NULL},
                                             &(BundleFlaws){.wrong_blake3 = &f->app});
    g_autoptr(GError) error = NULL;
    g_autoptr(MfBundle) bundle = mft_open_bundle(path, &error);

    g_assert_no_error(error);
    g_assert_false(mf_bundle_verify_image(bundle, mft_find_image(bundle, TEST_APP_ID), NULL, NULL, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_VERIFICATION_FAILED);
    g_assert_cmpstr(error->message,
                    ==,
                    "Image " TEST_APP_ID "_0.1.0.raw does not match what its manifest records");
    g_clear_error(&error);
    g_assert_true(mf_bundle_verify_image(bundle, mft_find_image(bundle, TEST_KIT_ID), NULL, NULL, &error));
    g_assert_no_error(error);
}

/**
 * test_unlisted_file:
 */
static void
test_unlisted_file(Fixture *f, gconstpointer user_data)
{
    g_autofree char *path = mft_write_bundle(f,
                                             "extra.manifold",
                                             (const TestImage *[]){&f->kit, &f->app, NULL},
                                             &(BundleFlaws){.unlisted_file = "evil.sh"});
    g_autoptr(GError) error = NULL;
    g_autoptr(MfBundle) bundle = mft_open_bundle(path, &error);

    g_assert_null(bundle);
    g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_BUNDLE);
    g_assert_cmpstr(error->message, ==, "The bundle contains evil.sh, which its index does not list");
}

/**
 * test_invalid_path:
 *
 * Only the catalog has directories, and only plain names in them.
 */
static void
test_invalid_path(Fixture *f, gconstpointer user_data)
{
    const char *names[] = {"catalog/../evil.sh", "catalog/.hidden", "catalog//x", "lib/evil.sh"};

    for (size_t i = 0; i < G_N_ELEMENTS(names); i++) {
        g_autofree char *path = mft_write_bundle(f,
                                                 "path.manifold",
                                                 (const TestImage *[]){&f->kit, &f->app, NULL},
                                                 &(BundleFlaws){.unlisted_file = names[i]});
        g_autoptr(GError) error = NULL;
        g_autoptr(MfBundle) bundle = mft_open_bundle(path, &error);

        g_assert_null(bundle);
        g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_BUNDLE);
        g_assert_cmpstr(error->message, ==, "The bundle contains a file with an invalid name");
    }
}

/**
 * test_undescribed_image:
 */
static void
test_undescribed_image(Fixture *f, gconstpointer user_data)
{
    g_autofree char *path = mft_write_bundle(f,
                                             "undescribed.manifold",
                                             (const TestImage *[]){&f->kit, &f->app, NULL},
                                             &(BundleFlaws){.undescribed = &f->kit});
    g_autoptr(GError) error = NULL;
    g_autoptr(MfBundle) bundle = mft_open_bundle(path, &error);

    g_assert_null(bundle);
    g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_BUNDLE);
    g_assert_cmpstr(error->message,
                    ==,
                    "catalog/manifold-local.xml does not describe x86_64/" TEST_KIT_ID "/20260112T0238");
}

/**
 * test_misdescribed_image:
 */
static void
test_misdescribed_image(Fixture *f, gconstpointer user_data)
{
    g_autofree char *path = mft_write_bundle(f,
                                             "misdescribed.manifold",
                                             (const TestImage *[]){&f->kit, &f->app, NULL},
                                             &(BundleFlaws){.misdescribed = &f->kit});
    g_autoptr(GError) error = NULL;
    g_autoptr(MfBundle) bundle = mft_open_bundle(path, &error);

    g_assert_null(bundle);
    g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_BUNDLE);
    g_assert_cmpstr(error->message,
                    ==,
                    "catalog/manifold-local.xml describes org.example.Other, not " TEST_KIT_ID);
}

/**
 * test_bad_icon:
 */
static void
test_bad_icon(Fixture *f, gconstpointer user_data)
{
    g_autofree char *path = mft_write_bundle(f,
                                             "icon.manifold",
                                             (const TestImage *[]){&f->kit, &f->app, NULL},
                                             &(BundleFlaws){.bad_icon = TRUE});
    g_autoptr(GError) error = NULL;
    g_autoptr(MfBundle) bundle = mft_open_bundle(path, &error);

    g_assert_null(bundle);
    g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_BUNDLE);
    g_assert_cmpstr(error->message, ==, "catalog/icons/64x64/" TEST_APP_ID ".jxl is not a JPEG XL image");
}

/**
 * test_corrupt_media:
 *
 * Screenshots are only hashed, but they are checked as well.
 */
static void
test_corrupt_media(Fixture *f, gconstpointer user_data)
{
    g_autofree char *path = mft_write_bundle(f,
                                             "media.manifold",
                                             (const TestImage *[]){&f->kit, &f->app, NULL},
                                             &(BundleFlaws){.corrupt_media = TRUE});
    g_autoptr(GError) error = NULL;
    g_autoptr(MfBundle) bundle = mft_open_bundle(path, &error);

    g_assert_null(bundle);
    g_assert_error(error, MF_ERROR, MF_ERROR_VERIFICATION_FAILED);
    g_assert_cmpstr(error->message,
                    ==,
                    "catalog/media/" TEST_APP_ID "/screenshot.jxl does not match what the index records");
}

/**
 * test_kit_without_base:
 */
static void
test_kit_without_base(Fixture *f, gconstpointer user_data)
{
    TestImage kit = f->kit;
    g_autofree char *path = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(MfBundle) bundle = NULL;

    kit.base = NULL;
    path = mft_write_bundle(f, "no-base.manifold", (const TestImage *[]){&kit, NULL}, &(BundleFlaws){});
    bundle = mft_open_bundle(path, &error);

    g_assert_null(bundle);
    g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_MANIFEST);
    g_assert_cmpstr(error->message,
                    ==,
                    TEST_KIT_ID "_20260112T0238.manifold.json: a kit must name the layer it is built on");
}

/**
 * test_one_id_two_kinds:
 *
 * An ID names one kind of component.
 */
static void
test_one_id_two_kinds(Fixture *f, gconstpointer user_data)
{
    TestImage app = f->app;
    g_autofree char *path = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(MfBundle) bundle = NULL;

    app.component_id = TEST_KIT_ID;
    path = mft_write_bundle(f,
                            "two-kinds.manifold",
                            (const TestImage *[]){&f->kit, &app, NULL},
                            &(BundleFlaws){});
    bundle = mft_open_bundle(path, &error);

    g_assert_null(bundle);
    g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_BUNDLE);
    g_assert_cmpstr(error->message,
                    ==,
                    "The bundle contains " TEST_KIT_ID " as two different kinds of component");
}

/**
 * test_not_a_bundle:
 */
static void
test_not_a_bundle(Fixture *f, gconstpointer user_data)
{
    g_autofree char *path = g_build_filename(f->dir, "notes.txt", NULL);
    g_autoptr(GError) error = NULL;
    g_autoptr(MfBundle) bundle = NULL;

    mft_write_file(path, "hello\n");
    bundle = mft_open_bundle(path, &error);

    g_assert_null(bundle);
    g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_BUNDLE);
}

/**
 * test_no_index:
 */
static void
test_no_index(Fixture *f, gconstpointer user_data)
{
    g_autofree char *path = g_build_filename(f->dir, "no-index.manifold", NULL);
    zip_t *zip = zip_open(path, ZIP_CREATE | ZIP_TRUNCATE, NULL);
    g_autoptr(GError) error = NULL;
    g_autoptr(MfBundle) bundle = NULL;

    mft_bundle_add(zip, "readme", "x", 1);
    g_assert_cmpint(zip_close(zip), ==, 0);
    bundle = mft_open_bundle(path, &error);

    g_assert_null(bundle);
    g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_BUNDLE);
    g_assert_cmpstr(error->message, ==, "The bundle has no index.json");
}

/**
 * test_metadata_limit:
 *
 * A bundle can not make us read more than 64 MiB of metadata, even if each
 * file is small enough on its own.
 */
static void
test_metadata_limit(Fixture *f, gconstpointer user_data)
{
    const gsize filler_size = 16 * 1024 * 1024;
    g_autofree char *path = g_build_filename(f->dir, "large.manifold", NULL);
    g_autofree guint8 *filler = g_malloc0(filler_size);
    g_autofree char *sha256 = g_compute_checksum_for_data(G_CHECKSUM_SHA256, filler, filler_size);
    g_autofree char *blake3 = mft_blake3_of(filler, filler_size);
    g_autoptr(GString) index = g_string_new("{\"format\": 1, \"files\": [");
    zip_t *zip = zip_open(path, ZIP_CREATE | ZIP_TRUNCATE, NULL);
    g_autoptr(GError) error = NULL;
    g_autoptr(MfBundle) bundle = NULL;

    /* with the index, the fourth file is too much */
    for (guint i = 0; i < 4; i++) {
        g_autofree char *name = g_strdup_printf("filler-%u", i);

        g_string_append_printf(index,
                               "%s{\"name\": \"%s\", \"size\": %zu, \"sha256\": \"%s\", \"blake3\": \"%s\"}",
                               i == 0 ? "" : ", ",
                               name,
                               filler_size,
                               sha256,
                               blake3);
        mft_bundle_add(zip, name, filler, filler_size);
    }
    g_string_append(index, "]}");
    mft_bundle_add(zip, "index.json", index->str, index->len);
    g_assert_cmpint(zip_close(zip), ==, 0);
    bundle = mft_open_bundle(path, &error);

    g_assert_null(bundle);
    g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_BUNDLE);
    g_assert_cmpstr(error->message, ==, "The metadata of the bundle is larger than 64 MiB altogether");
}

/**
 * mft_read_zip_entry:
 *
 * Returns: (transfer full): the content of a file in a bundle
 */
static GBytes *
mft_read_zip_entry(const char *path, const char *name)
{
    zip_t *zip = zip_open(path, ZIP_RDONLY, NULL);
    zip_stat_t st;
    zip_file_t *file;
    char *data;

    g_assert_nonnull(zip);
    g_assert_cmpint(zip_stat(zip, name, 0, &st), ==, 0);
    file = zip_fopen(zip, name, 0);
    g_assert_nonnull(file);
    data = g_malloc(st.size);
    g_assert_cmpint(zip_fread(file, data, st.size), ==, st.size);
    zip_fclose(file);
    zip_discard(zip);

    return g_bytes_new_take(data, st.size);
}

/**
 * test_sign_file:
 */
static void
test_sign_file(Fixture *f, gconstpointer user_data)
{
    g_autofree char *ssh_keygen = g_find_program_in_path("ssh-keygen");
    g_autofree char *path = NULL;
    g_autofree char *key = NULL;
    g_autofree char *other_key = NULL;
    g_autoptr(MfSigner) signer = NULL;
    g_autoptr(MfSigner) other_signer = NULL;
    g_autoptr(MfBundle) bundle = NULL;
    g_autoptr(GBytes) index = NULL;
    g_autoptr(GBytes) signature = NULL;
    g_autoptr(GBytes) new_signature = NULL;
    g_autoptr(GError) error = NULL;

    if (ssh_keygen == NULL) {
        g_test_skip("ssh-keygen is not installed");
        return;
    }
    path = mft_write_bundle(f,
                            "unsigned.manifold",
                            (const TestImage *[]){&f->kit, &f->app, NULL},
                            &(BundleFlaws){.unsigned_ = TRUE});
    key = mft_make_key(f->dir, "key");
    signer = mf_signer_new_for_key_file(key);

    g_assert_true(mf_bundle_sign_file(path, signer, &error));
    g_assert_no_error(error);

    bundle = mft_open_bundle(path, &error);
    g_assert_no_error(error);
    g_assert_true(mf_bundle_is_signed(bundle));

    index = mft_read_zip_entry(path, "index.json");
    signature = mft_read_zip_entry(path, "index.json.sig");
    mft_check_signature(f->dir, index, signature);

    /* signing again replaces the signature */
    other_key = mft_make_key(f->dir, "other-key");
    other_signer = mf_signer_new_for_key_file(other_key);
    g_assert_true(mf_bundle_sign_file(path, other_signer, &error));
    g_assert_no_error(error);

    new_signature = mft_read_zip_entry(path, "index.json.sig");
    g_assert_false(g_bytes_equal(signature, new_signature));
    mft_check_signature(f->dir, index, new_signature);
}

/**
 * test_sign_tampered_file:
 *
 * A bundle that we would not open must not be signed.
 */
static void
test_sign_tampered_file(Fixture *f, gconstpointer user_data)
{
    g_autofree char *ssh_keygen = g_find_program_in_path("ssh-keygen");
    g_autofree char *path = NULL;
    g_autofree char *key = NULL;
    g_autoptr(MfSigner) signer = NULL;
    g_autoptr(GError) error = NULL;
    zip_t *zip;

    if (ssh_keygen == NULL) {
        g_test_skip("ssh-keygen is not installed");
        return;
    }
    path = mft_write_bundle(f,
                            "tampered.manifold",
                            (const TestImage *[]){&f->kit, &f->app, NULL},
                            &(BundleFlaws){.unsigned_ = TRUE, .tampered = &f->app});
    key = mft_make_key(f->dir, "key");
    signer = mf_signer_new_for_key_file(key);

    g_assert_false(mf_bundle_sign_file(path, signer, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_VERIFICATION_FAILED);

    zip = zip_open(path, ZIP_RDONLY, NULL);
    g_assert_nonnull(zip);
    g_assert_cmpint(zip_name_locate(zip, "index.json.sig", 0), <, 0);
    zip_discard(zip);
}

/**
 * mft_image_catalog:
 * @extra: (nullable): more XML for the component
 *
 * Returns: (transfer full): catalog XML for an image. Its origin and the image
 *   it names are wrong, as they are up to the writer.
 */
static GBytes *
mft_image_catalog(const char *component_id, const char *extra)
{
    g_autofree char *component = mft_catalog_component_xml(component_id, "x86_64/wrong/1", extra);
    char *xml = g_strdup_printf("<components version=\"1.0\" origin=\"other\">%s</components>", component);

    return g_bytes_new_take(xml, strlen(xml));
}

/**
 * mft_write_image_file:
 *
 * Returns: (transfer full): the path of the file of an image
 */
static char *
mft_write_image_file(Fixture *f, const TestImage *image)
{
    g_autofree char *file_name = mft_image_file_name(image);
    char *path = g_build_filename(f->dir, file_name, NULL);

    g_assert_true(
        g_file_set_contents(path, g_bytes_get_data(image->data, NULL), g_bytes_get_size(image->data), NULL));
    return path;
}

/**
 * mft_writer_add_image:
 * @extra: (nullable): more XML for the component
 */
static void
mft_writer_add_image(Fixture *f, MfBundleWriter *writer, const TestImage *image, const char *extra)
{
    g_autofree char *path = mft_write_image_file(f, image);
    char *json = mft_image_manifest_json(image, FALSE);
    g_autoptr(GBytes) manifest = g_bytes_new_take(json, strlen(json));
    g_autoptr(GBytes) catalog = mft_image_catalog(image->component_id, extra);
    g_autoptr(GError) error = NULL;

    g_assert_true(mf_bundle_writer_add_image(writer, path, manifest, catalog, &error));
    g_assert_no_error(error);
}

/**
 * mft_new_writer:
 *
 * Returns: (transfer full): a writer for a bundle in the test directory, with the kit
 */
static MfBundleWriter *
mft_new_writer(Fixture *f, const char *path)
{
    g_autoptr(GError) error = NULL;
    MfBundleWriter *writer = mf_bundle_writer_new(path, &error);

    g_assert_no_error(error);
    mft_writer_add_image(f, writer, &f->kit, NULL);
    return writer;
}

/**
 * test_writer:
 */
static void
test_writer(Fixture *f, gconstpointer user_data)
{
    g_autofree char *path = g_build_filename(f->dir, "written.manifold", NULL);
    g_autofree char *icons = g_build_filename(f->dir, "icons", NULL);
    g_autofree char *media = g_build_filename(f->dir, "media", NULL);
    g_autofree char *icon_dir = g_build_filename(icons, "64x64", NULL);
    g_autofree char *icon = g_build_filename(icon_dir, TEST_APP_ID ".jxl", NULL);
    g_autofree char *media_dir = g_build_filename(media, TEST_APP_ID, NULL);
    g_autofree char *screenshot = g_build_filename(media_dir, "screenshot.jxl", NULL);
    g_autofree char *extra = g_strdup_printf("<icon type=\"cached\" width=\"64\" height=\"64\">%s.jxl</icon>",
                                             TEST_APP_ID);
    g_autoptr(MfBundleWriter) writer = mft_new_writer(f, path);
    g_autoptr(MfBundle) bundle = NULL;
    g_autoptr(GBytes) catalog = NULL;
    g_autoptr(GBytes) index = NULL;
    g_autoptr(GError) error = NULL;
    MfBundleImage *app;
    gsize size;
    const char *xml;

    mft_writer_add_image(f, writer, &f->app, extra);

    mft_make_dirs(icon_dir);
    mft_make_dirs(media_dir);
    g_assert_true(g_file_set_contents(icon, (const char *) JXL_DATA, sizeof(JXL_DATA), NULL));
    g_assert_true(g_file_set_contents(screenshot, (const char *) JXL_DATA, sizeof(JXL_DATA), NULL));
    g_assert_true(mf_bundle_writer_add_catalog_assets(writer, icons, media, &error));
    g_assert_no_error(error);

    /* nothing reaches the disk before we finish */
    g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
    g_assert_true(mf_bundle_writer_finish(writer, NULL, &error));
    g_assert_no_error(error);

    /* what we write is what we read */
    bundle = mft_open_bundle(path, &error);
    g_assert_no_error(error);
    g_assert_false(mf_bundle_is_signed(bundle));
    g_assert_cmpuint(mf_bundle_get_images(bundle)->len, ==, 2);

    app = mft_find_image(bundle, TEST_APP_ID);
    g_assert_cmpstr(mf_bundle_image_get_manifest_name(app), ==, TEST_APP_ID "_0.1.0.manifold.json");
    g_assert_cmpuint(g_hash_table_size(mf_bundle_image_get_icons(app)), ==, 1);
    g_assert_true(mf_bundle_verify_image(bundle, app, NULL, NULL, &error));
    g_assert_no_error(error);

    /* the catalog has our origin, and opening the bundle checked the image it names */
    catalog = mft_read_zip_entry(path, "catalog/manifold-local.xml");
    xml = g_bytes_get_data(catalog, &size);
    g_assert_nonnull(g_strstr_len(xml, size, "origin=\"manifold-local\""));
    g_assert_null(g_strstr_len(xml, size, "x86_64/wrong/1"));

    /* the manifests record the images, so the index does not list them */
    index = mft_read_zip_entry(path, "index.json");
    xml = g_bytes_get_data(index, &size);
    g_assert_nonnull(g_strstr_len(xml, size, TEST_APP_ID "_0.1.0.manifold.json"));
    g_assert_null(g_strstr_len(xml, size, TEST_APP_ID "_0.1.0.raw"));
}

/**
 * test_writer_sign:
 *
 * If signing fails nothing is written, and we can try again.
 */
static void
test_writer_sign(Fixture *f, gconstpointer user_data)
{
    g_autofree char *ssh_keygen = g_find_program_in_path("ssh-keygen");
    g_autofree char *path = g_build_filename(f->dir, "written.manifold", NULL);
    g_autofree char *bad_key = g_build_filename(f->dir, "bad-key", NULL);
    g_autofree char *key = NULL;
    g_autoptr(MfBundleWriter) writer = NULL;
    g_autoptr(MfSigner) bad_signer = NULL;
    g_autoptr(MfSigner) signer = NULL;
    g_autoptr(GBytes) index = NULL;
    g_autoptr(GBytes) signature = NULL;
    g_autoptr(GError) error = NULL;

    if (ssh_keygen == NULL) {
        g_test_skip("ssh-keygen is not installed");
        return;
    }
    mft_write_file(bad_key, "not a key\n");
    bad_signer = mf_signer_new_for_key_file(bad_key);
    key = mft_make_key(f->dir, "key");
    signer = mf_signer_new_for_key_file(key);

    writer = mft_new_writer(f, path);
    g_assert_false(mf_bundle_writer_finish(writer, bad_signer, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));

    g_assert_true(mf_bundle_writer_finish(writer, signer, &error));
    g_assert_no_error(error);

    index = mft_read_zip_entry(path, "index.json");
    signature = mft_read_zip_entry(path, "index.json.sig");
    mft_check_signature(f->dir, index, signature);
}

/**
 * test_writer_refused:
 *
 * We refuse what would make a bundle unreadable, and that changes nothing:
 * the bundle we get in the end only holds what was accepted.
 */
static void
test_writer_refused(Fixture *f, gconstpointer user_data)
{
    g_autofree char *path = g_build_filename(f->dir, "written.manifold", NULL);
    g_autofree char *app_file = mft_write_image_file(f, &f->app);
    g_autofree char *kit_file = mft_write_image_file(f, &f->kit);
    g_autofree char *app_xml = mft_catalog_component_xml(TEST_APP_ID, NULL, NULL);
    g_autofree char *kit_xml = mft_catalog_component_xml(TEST_KIT_ID, NULL, NULL);
    g_autofree char *both_xml = g_strconcat("<components>", app_xml, kit_xml, "</components>", NULL);
    g_autofree char *icons = g_build_filename(f->dir, "icons", NULL);
    g_autofree char *good_dir = g_build_filename(icons, "a", NULL);
    g_autofree char *bad_dir = g_build_filename(icons, "b", NULL);
    g_autofree char *good_file = g_build_filename(good_dir, "icon", NULL);
    g_autofree char *hidden_file = g_build_filename(bad_dir, ".hidden", NULL);
    char *json = mft_image_manifest_json(&f->app, FALSE);
    g_autoptr(GBytes) manifest = g_bytes_new_take(json, strlen(json));
    g_autoptr(GBytes) catalog = mft_image_catalog(TEST_APP_ID, NULL);
    g_autoptr(GBytes) kit_catalog = mft_image_catalog(TEST_KIT_ID, NULL);
    g_autoptr(GBytes) both_catalog = g_bytes_new_static(both_xml, strlen(both_xml));
    g_autoptr(MfBundleWriter) writer = mf_bundle_writer_new(path, NULL);
    g_autoptr(MfBundle) bundle = NULL;
    g_autoptr(GError) error = NULL;

    g_assert_false(mf_bundle_writer_finish(writer, NULL, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_BUNDLE);
    g_assert_cmpstr(error->message, ==, "The bundle contains no images");
    g_clear_error(&error);

    /* the manifest describes another image */
    g_assert_false(mf_bundle_writer_add_image(writer, kit_file, manifest, catalog, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_VERIFICATION_FAILED);
    g_clear_error(&error);

    g_assert_false(mf_bundle_writer_add_image(writer, app_file, manifest, kit_catalog, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_BUNDLE);
    g_assert_cmpstr(error->message, ==, "The catalog data of " TEST_APP_ID " describes " TEST_KIT_ID);
    g_clear_error(&error);

    g_assert_false(mf_bundle_writer_add_image(writer, app_file, manifest, both_catalog, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_BUNDLE);
    g_clear_error(&error);

    g_assert_true(mf_bundle_writer_add_image(writer, app_file, manifest, catalog, &error));
    g_assert_no_error(error);
    g_assert_false(mf_bundle_writer_add_image(writer, app_file, manifest, catalog, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_BUNDLE);
    g_clear_error(&error);

    /* the first file of the tree is fine, but it must not stay if a later one is not */
    mft_make_dirs(good_dir);
    mft_make_dirs(bad_dir);
    mft_write_file(good_file, "icon");
    mft_write_file(hidden_file, "hidden");
    g_assert_false(mf_bundle_writer_add_catalog_assets(writer, icons, NULL, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_INVALID_BUNDLE);
    g_clear_error(&error);
    g_assert_cmpint(g_remove(hidden_file), ==, 0);
    g_assert_true(mf_bundle_writer_add_catalog_assets(writer, icons, NULL, &error));
    g_assert_no_error(error);

    g_assert_true(mf_bundle_writer_finish(writer, NULL, &error));
    g_assert_no_error(error);
    bundle = mft_open_bundle(path, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(mf_bundle_get_images(bundle)->len, ==, 1);
}

/**
 * main:
 */
int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

#define add_test(name, func) g_test_add("/bundle/" name, Fixture, NULL, mft_setup, func, mft_teardown)
    add_test("valid", test_valid_bundle);
    add_test("core-has-no-base", test_core_has_no_base);
    add_test("copy-image", test_copy_image);
    add_test("corrupt-image", test_corrupt_image);
    add_test("missing-image", test_missing_image);
    add_test("listed-twice", test_listed_twice);
    add_test("tampered-manifest", test_tampered_manifest);
    add_test("wrong-blake3", test_wrong_blake3);
    add_test("unlisted-file", test_unlisted_file);
    add_test("invalid-path", test_invalid_path);
    add_test("undescribed-image", test_undescribed_image);
    add_test("misdescribed-image", test_misdescribed_image);
    add_test("bad-icon", test_bad_icon);
    add_test("corrupt-media", test_corrupt_media);
    add_test("kit-without-base", test_kit_without_base);
    add_test("one-id-two-kinds", test_one_id_two_kinds);
    add_test("not-a-bundle", test_not_a_bundle);
    add_test("no-index", test_no_index);
    add_test("metadata-limit", test_metadata_limit);
    add_test("sign-file", test_sign_file);
    add_test("sign-tampered-file", test_sign_tampered_file);
    add_test("writer", test_writer);
    add_test("writer-sign", test_writer_sign);
    add_test("writer-refused", test_writer_refused);
#undef add_test

    return g_test_run();
}
