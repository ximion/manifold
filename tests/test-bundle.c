/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>
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
} BundleFlaws;

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

    return mft_manifest_json(&(MftManifest){
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
        g_autofree char *metainfo_name = g_strconcat(base_name, ".metainfo.xml", NULL);
        g_autofree char *manifest = mft_image_manifest_json(image, image == flaws->wrong_blake3);
        g_autofree char *metainfo = mft_metainfo_xml(image->component_id);
        const char *const names[] = {manifest_name, metainfo_name};
        const char *const contents[] = {manifest, metainfo};
        gsize data_size;
        const guint8 *data = g_bytes_get_data(image->data, &data_size);

        for (size_t j = 0; j < G_N_ELEMENTS(names); j++) {
            g_autofree char *sha256 = g_compute_checksum_for_string(G_CHECKSUM_SHA256, contents[j], -1);
            g_autofree char *blake3 = mft_blake3_of(contents[j], strlen(contents[j]));

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
                names[j],
                strlen(contents[j]),
                sha256,
                blake3);
            /* clang-format on */
        }
        if (image == flaws->tampered) {
            g_autofree char *changed = g_strconcat(manifest, "\n", NULL);

            mft_bundle_add(zip, manifest_name, changed, strlen(changed));
        } else {
            mft_bundle_add(zip, manifest_name, manifest, strlen(manifest));
        }
        mft_bundle_add(zip, metainfo_name, metainfo, strlen(metainfo));

        if (image == flaws->corrupt) {
            g_autofree guint8 *changed = g_memdup2(data, data_size);

            changed[0] ^= 0xff;
            mft_bundle_add(zip, file_name, changed, data_size);
        } else if (image != flaws->missing) {
            mft_bundle_add(zip, file_name, data, data_size);
        }
    }
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

static void
mft_count_progress(guint64 bytes_copied, gpointer user_data)
{
    guint64 *last = user_data;

    g_assert_cmpuint(bytes_copied, >, *last);
    *last = bytes_copied;
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
    g_assert_nonnull(mf_bundle_image_get_metainfo(app));
    g_assert_true(mf_manifest_has_image(manifest));
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
    g_autofree char *path = mft_write_bundle(f,
                                             "app.manifold",
                                             (const TestImage *[]){&f->kit, &f->app, NULL},
                                             &(BundleFlaws){});
    g_autofree char *copy_path = g_build_filename(f->dir, "copy.raw", NULL);
    g_autoptr(GError) error = NULL;
    g_autoptr(MfBundle) bundle = mft_open_bundle(path, &error);
    g_autofree char *copied = NULL;
    gsize copied_size;
    guint64 last_progress = 0;
    int fd;

    g_assert_no_error(error);
    fd = open(copy_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    g_assert_cmpint(fd, >=, 0);
    g_assert_true(mf_bundle_copy_image(bundle,
                                       mft_find_image(bundle, TEST_APP_ID),
                                       fd,
                                       mft_count_progress,
                                       &last_progress,
                                       &error));
    g_assert_no_error(error);
    close(fd);

    g_assert_cmpuint(last_progress, ==, 12288);
    g_assert_true(g_file_get_contents(copy_path, &copied, &copied_size, NULL));
    g_assert_cmpmem(copied, copied_size, g_bytes_get_data(f->app.data, NULL), 12288);
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
                    "Image " TEST_KIT_ID "_20260112T0238.raw of " TEST_KIT_ID
                    "_20260112T0238.manifold.json is not in the bundle");
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
    add_test("tampered-manifest", test_tampered_manifest);
    add_test("wrong-blake3", test_wrong_blake3);
    add_test("unlisted-file", test_unlisted_file);
    add_test("kit-without-base", test_kit_without_base);
    add_test("not-a-bundle", test_not_a_bundle);
    add_test("no-index", test_no_index);
#undef add_test

    return g_test_run();
}
