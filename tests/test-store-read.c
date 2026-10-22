/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <glib/gstdio.h>
#include <manifold.h>

#include "mf-component-private.h"
#include "mf-store-private.h"
#include "mft-utils.h"

typedef struct {
    char *dir;
    MfStore *store;
} Fixture;

/**
 * mft_assert_read_fails:
 *
 * Assert that reading a component directly fails with the given error.
 */
static void
mft_assert_read_fails(Fixture *f, const char *component_id, int code, const char *message)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(MfComponent) component = mf_store_read_component_at(f->dir, component_id, NULL, &error);

    g_assert_null(component);
    g_assert_error(error, MF_ERROR, code);
    if (message != NULL)
        g_assert_cmpstr(error->message, ==, message);
}

static void
mft_write_runtime(Fixture *f, const MftTestManifest *manifest, const char *base_version_dir, const char *mode)
{
    mft_write_version(f->dir, MF_STORE_RUNTIMES_SUBDIR, manifest, base_version_dir, mode);
}

static void
mft_setup(Fixture *f, gconstpointer user_data)
{
    f->dir = mft_make_tmp_dir("store");
    f->store = mf_store_new_for_path(f->dir);

    /* two versions of the core, the SDK of the older one, a platform on the newer core,
     * and an app on the platform */
    mft_write_runtime(f,
                      &(MftTestManifest){.component_id = TEST_CORE_ID, .kind = "core", .version = "13.4"},
                      NULL,
                      "manual");
    mft_write_runtime(f,
                      &(MftTestManifest){.component_id = TEST_CORE_ID,
                                         .kind = "core",
                                         .version = "13.8",
                                         .revision = 22810000},
                      NULL,
                      "manual");
    mft_write_runtime(f,
                      &(MftTestManifest){.component_id = TEST_SDK_ID,
                                         .kind = "core",
                                         .version = "13.4",
                                         .sdk = TRUE,
                                         .base_id = TEST_CORE_ID},
                      "13.4-1",
                      "manual");
    mft_write_runtime(f,
                      &(MftTestManifest){.component_id = TEST_PLATFORM_ID,
                                         .kind = "platform",
                                         .version = "13.8",
                                         .revision = 22910001,
                                         .base_id = TEST_CORE_ID},
                      "13.8-22810000",
                      "auto");
    mft_write_version(f->dir,
                      MF_STORE_APPS_SUBDIR,
                      &(MftTestManifest){.component_id = TEST_APP_ID,
                                         .kind = "app",
                                         .version = "0.1.0",
                                         .base_id = TEST_PLATFORM_ID},
                      NULL,
                      NULL);
}

static void
mft_teardown(Fixture *f, gconstpointer user_data)
{
    mft_rm_rf(f->dir);
    g_clear_object(&f->store);
    g_free(f->dir);
}

/**
 * test_get_current:
 *
 * Tests getting the current component.
 */
static void
test_get_current(Fixture *f, gconstpointer user_data)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(MfComponent) core = mf_store_get_component(f->store, TEST_CORE_ID, NULL, &error);
    g_autoptr(MfComponent) app = NULL;
    g_autofree char *core_hash = mft_fake_hash(TEST_CORE_ID);
    g_autofree char *image = NULL;
    g_autofree char *expected_image = g_strdup_printf("%s/images/by-verity/%s.raw", f->dir, core_hash);

    g_assert_no_error(error);
    g_assert_cmpstr(mf_component_get_id(core), ==, TEST_CORE_ID);
    g_assert_cmpint(mf_component_get_kind(core), ==, MF_COMPONENT_KIND_CORE);
    g_assert_cmpstr(mf_component_get_version(core), ==, "13.8");
    g_assert_cmpuint(mf_component_get_revision(core), ==, 22810000);
    g_assert_cmpstr(mf_component_get_verity_hash(core), ==, core_hash);
    g_assert_true(mf_component_is_current(core));
    g_assert_cmpint(mf_component_get_install_mode(core), ==, MF_INSTALL_MODE_MANUAL);
    g_assert_cmpstr(mf_component_get_origin(core), ==, "local");

    g_assert_true(g_str_has_suffix(mf_component_get_dir(core), "/runtimes/" TEST_CORE_ID "/13.8-22810000"));
    image = mf_store_image_path_at(mf_store_get_path(f->store), mf_component_get_verity_hash(core));
    g_assert_cmpstr(image, ==, expected_image);

    /* apps are found too, in apps/ instead of runtimes/ */
    app = mf_store_get_component(f->store, TEST_APP_ID, NULL, &error);
    g_assert_no_error(error);
    g_assert_cmpint(mf_component_get_kind(app), ==, MF_COMPONENT_KIND_APP);
}

/**
 * test_get_version:
 *
 * Tests getting a specific version of a component.
 */
static void
test_get_version(Fixture *f, gconstpointer user_data)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(MfComponent) old = mf_store_get_component(f->store, TEST_CORE_ID, "13.4", &error);
    g_autoptr(MfComponent) platform = NULL;
    g_autoptr(MfManifest) manifest = NULL;

    g_assert_no_error(error);
    g_assert_cmpstr(mf_component_get_version(old), ==, "13.4");
    g_assert_cmpuint(mf_component_get_revision(old), ==, 1);
    g_assert_false(mf_component_is_current(old));

    /* the install mode is read as well, and the manifest is the one of that version */
    platform = mf_store_get_component(f->store, TEST_PLATFORM_ID, "13.8", &error);
    g_assert_no_error(error);
    g_assert_cmpint(mf_component_get_install_mode(platform), ==, MF_INSTALL_MODE_AUTO);
    manifest = mf_component_fetch_manifest(platform, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(mf_manifest_get_base_id(manifest), ==, TEST_CORE_ID);
    g_assert_cmpuint(mf_manifest_get_data_offset(manifest), ==, 20480);

    g_assert_null(mf_store_get_component(f->store, TEST_CORE_ID, "14", &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_NOT_FOUND);
    g_clear_error(&error);
    g_assert_null(mf_store_get_component(f->store, "org.example.nothing", NULL, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_NOT_FOUND);
}

/**
 * test_get_base:
 *
 * Tests getting the base component.
 */
static void
test_get_base(Fixture *f, gconstpointer user_data)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(MfComponent) app = mf_store_get_component(f->store, TEST_APP_ID, NULL, &error);
    g_autoptr(MfComponent) platform = NULL;
    g_autoptr(MfComponent) core = NULL;
    g_autoptr(MfComponent) sdk = NULL;
    g_autoptr(MfComponent) sdk_base = NULL;

    g_assert_no_error(error);
    /* follow the base links: the app's link names the current version of the platform,
     * and the platform's link names an exact version of the core */
    platform = mf_store_get_base(f->store, app, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(mf_component_get_id(platform), ==, TEST_PLATFORM_ID);
    core = mf_store_get_base(f->store, platform, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(mf_component_get_id(core), ==, TEST_CORE_ID);
    g_assert_cmpstr(mf_component_get_version(core), ==, "13.8");
    g_assert_null(mf_store_get_base(f->store, core, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_NOT_FOUND);
    g_assert_cmpstr(error->message, ==, TEST_CORE_ID " 13.8 has no base");
    g_clear_error(&error);

    /* the base of the core SDK is the exact core version it was built for, even if that is
     * not the current one */
    sdk = mf_store_get_component(f->store, TEST_SDK_ID, NULL, &error);
    g_assert_no_error(error);
    g_assert_true(mf_component_is_sdk(sdk));
    sdk_base = mf_store_get_base(f->store, sdk, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(mf_component_get_id(sdk_base), ==, TEST_CORE_ID);
    g_assert_cmpstr(mf_component_get_version(sdk_base), ==, "13.4");
    g_assert_false(mf_component_is_current(sdk_base));
}

/**
 * test_base_link_must_name_a_component:
 */
static void
test_base_link_must_name_a_component(Fixture *f, gconstpointer user_data)
{
    g_autofree char *link = g_build_filename(f->dir, "apps", TEST_APP_ID, "0.1.0-1", "base", NULL);
    g_autofree char *expected = g_strdup_printf("Link %s does not point into the store: /etc", link);
    g_autoptr(GError) error = NULL;
    g_autoptr(MfComponent) app = mf_store_get_component(f->store, TEST_APP_ID, NULL, &error);

    g_assert_no_error(error);

    /* a link that leaves the store is refused */
    mft_replace_link("/etc", link);
    g_assert_null(mf_store_get_base(f->store, app, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_FAILED);
    g_assert_cmpstr(error->message, ==, expected);
    g_clear_error(&error);

    /* so is a link that takes a detour through "..", and one to a hidden component */
    mft_replace_link("../../../runtimes/" TEST_PLATFORM_ID "/../" TEST_PLATFORM_ID "/current", link);
    g_assert_null(mf_store_get_base(f->store, app, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_FAILED);
    g_clear_error(&error);
    mft_replace_link("../../../runtimes/.hidden/current", link);
    g_assert_null(mf_store_get_base(f->store, app, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_FAILED);
    g_clear_error(&error);

    /* a link to a component that is not installed */
    mft_replace_link("../../../runtimes/org.example.gone/current", link);
    g_assert_null(mf_store_get_base(f->store, app, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_NOT_FOUND);
}

/**
 * test_sdk_app_is_a_runtime:
 *
 * An SDK is installed as a runtime, even the SDK of an application, which
 * builds on that application.
 */
static void
test_sdk_app_is_a_runtime(Fixture *f, gconstpointer user_data)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(MfComponent) sdk = NULL;
    g_autoptr(MfComponent) base = NULL;

    mft_write_runtime(f,
                      &(MftTestManifest){.component_id = TEST_APP_SDK_ID,
                                         .kind = "app",
                                         .version = "0.1.0",
                                         .sdk = TRUE,
                                         .base_id = TEST_APP_ID},
                      NULL,
                      "manual");
    sdk = mf_store_get_component(f->store, TEST_APP_SDK_ID, NULL, &error);
    g_assert_no_error(error);
    g_assert_cmpint(mf_component_get_kind(sdk), ==, MF_COMPONENT_KIND_APP);
    g_assert_true(mf_component_is_sdk(sdk));

    g_assert_true(g_str_has_suffix(mf_component_get_dir(sdk), "/runtimes/" TEST_APP_SDK_ID "/0.1.0-1"));
    base = mf_store_get_base(f->store, sdk, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(mf_component_get_id(base), ==, TEST_APP_ID);
    g_assert_cmpint(mf_component_get_kind(base), ==, MF_COMPONENT_KIND_APP);
}

/**
 * test_list:
 *
 * Tests listing components.
 */
static void
test_list(Fixture *f, gconstpointer user_data)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *junk = g_build_filename(f->dir, "apps", TEST_APP_ID, "2.0-1", NULL);
    g_autoptr(GPtrArray) components = NULL;
    const struct {
        const char *component_id;
        const char *version;
        gboolean current;
    } expected[] = {
        {TEST_APP_ID,      "0.1.0", TRUE },
        {TEST_PLATFORM_ID, "13.8",  TRUE },
        {TEST_CORE_ID,     "13.4",  FALSE},
        {TEST_CORE_ID,     "13.8",  TRUE },
        {TEST_SDK_ID,      "13.4",  TRUE },
    };

    g_assert_cmpint(g_mkdir(junk, 0755), ==, 0); /* a version without image or manifest is skipped */
    components = mf_store_list_components(f->store, NULL, MF_COMPONENT_KIND_UNKNOWN, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(components->len, ==, G_N_ELEMENTS(expected));
    for (guint i = 0; i < components->len; i++) {
        MfComponent *component = g_ptr_array_index(components, i);

        g_assert_cmpstr(mf_component_get_id(component), ==, expected[i].component_id);
        g_assert_cmpstr(mf_component_get_version(component), ==, expected[i].version);
        g_assert_cmpint(mf_component_is_current(component), ==, expected[i].current);
    }

    /* the filters */
    g_clear_pointer(&components, g_ptr_array_unref);
    components = mf_store_list_components(f->store, TEST_CORE_ID, MF_COMPONENT_KIND_UNKNOWN, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(components->len, ==, 2);
    g_clear_pointer(&components, g_ptr_array_unref);
    components = mf_store_list_components(f->store, NULL, MF_COMPONENT_KIND_APP, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(components->len, ==, 1);
    g_assert_cmpstr(mf_component_get_id(g_ptr_array_index(components, 0)), ==, TEST_APP_ID);
    g_clear_pointer(&components, g_ptr_array_unref);
    components = mf_store_list_components(f->store, TEST_APP_ID, MF_COMPONENT_KIND_CORE, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(components->len, ==, 0);
}

/**
 * test_image_link_must_point_into_store:
 */
static void
test_image_link_must_point_into_store(Fixture *f, gconstpointer user_data)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *link = g_build_filename(f->dir, "apps", TEST_APP_ID, "0.1.0-1", "app.raw", NULL);
    g_autofree char *expected = NULL;

    /* reading the component directly fails; in the loaded store, it is simply not installed */
    expected = g_strdup_printf("Link %s does not point into the image store: /etc/passwd", link);
    mft_replace_link("/etc/passwd", link);
    mft_assert_read_fails(f, TEST_APP_ID, MF_ERROR_FAILED, expected);
    g_assert_null(mf_store_get_component(f->store, TEST_APP_ID, NULL, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_NOT_FOUND);

    /* a link that leaves the image store through "..", and one to a file whose name is not a root hash */
    mft_replace_link("../../../images/by-verity/../../../../etc/passwd.raw", link);
    mft_assert_read_fails(f, TEST_APP_ID, MF_ERROR_FAILED, NULL);
    g_free(expected);
    expected = g_strdup_printf(
        "Link %s does not name an image by its root hash: ../../../images/by-verity/EVIL.raw",
        link);
    mft_replace_link("../../../images/by-verity/EVIL.raw", link);
    mft_assert_read_fails(f, TEST_APP_ID, MF_ERROR_FAILED, expected);
}

/**
 * test_manifest_must_be_sound:
 *
 * A manifest the install daemon would have rejected is refused when reading.
 */
static void
test_manifest_must_be_sound(Fixture *f, gconstpointer user_data)
{
    g_autofree char *manifest = g_build_filename(f->dir,
                                                 "apps",
                                                 TEST_APP_ID,
                                                 "0.1.0-1",
                                                 "manifest.json",
                                                 NULL);
    g_autofree char *platform_manifest = g_build_filename(f->dir,
                                                          "runtimes",
                                                          TEST_PLATFORM_ID,
                                                          "13.8-22910001",
                                                          "manifest.json",
                                                          NULL);
    g_autofree char *expected = NULL;

    expected = g_strdup_printf("%s: image: the hash partition is placed before the data", manifest);
    mft_replace_in_file(manifest, "\"hash-offset\": 22528", "\"hash-offset\": 1");
    mft_assert_read_fails(f, TEST_APP_ID, MF_ERROR_INVALID_MANIFEST, expected);

    mft_write_file(manifest, "not json");
    mft_assert_read_fails(f, TEST_APP_ID, MF_ERROR_INVALID_MANIFEST, NULL);

    /* a manifest that names another component than the directory it is in */
    g_free(expected);
    expected = g_strdup_printf("%s says it is something else (expected: " TEST_PLATFORM_ID "/13.8)",
                               platform_manifest);
    mft_replace_in_file(platform_manifest,
                        "\"component\": \"" TEST_PLATFORM_ID "\"",
                        "\"component\": \"" TEST_APP_ID "\"");
    mft_assert_read_fails(f, TEST_PLATFORM_ID, MF_ERROR_FAILED, expected);
}

/**
 * mft_new_core:
 * @dir: (nullable): the version directory to make the component for
 *
 * Make the component of the current core, as the install daemon describes
 * it, without its manifest.
 */
static MfComponent *
mft_new_core(const char *dir)
{
    g_autofree char *core_hash = mft_fake_hash(TEST_CORE_ID);

    return mf_component_new(TEST_CORE_ID,
                            MF_COMPONENT_KIND_CORE,
                            "13.8",
                            22810000,
                            FALSE,
                            core_hash,
                            core_hash,
                            TRUE,
                            MF_INSTALL_MODE_MANUAL,
                            "local",
                            dir);
}

/**
 * test_fetch_manifest:
 */
static void
test_fetch_manifest(Fixture *f, gconstpointer user_data)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(MfComponent) core = mf_store_get_component(f->store, TEST_CORE_ID, NULL, &error);
    g_autoptr(MfComponent) unread = NULL;
    g_autoptr(MfComponent) removed = NULL;
    g_autoptr(MfComponent) reported = NULL;
    g_autoptr(MfManifest) manifest = NULL;
    g_autoptr(MfManifest) again = NULL;

    /* a component read from the store keeps the manifest the store read */
    g_assert_no_error(error);
    manifest = mf_component_fetch_manifest(core, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(mf_manifest_get_component_id(manifest), ==, TEST_CORE_ID);
    again = mf_component_fetch_manifest(core, &error);
    g_assert_no_error(error);
    g_assert_true(again == manifest);
    g_clear_object(&manifest);
    g_clear_object(&again);

    /* a component without a manifest reads it from its version directory once, and keeps it */
    unread = mft_new_core(mf_component_get_dir(core));
    manifest = mf_component_fetch_manifest(unread, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(mf_manifest_get_version(manifest), ==, "13.8");
    mft_rm_rf(mf_component_get_dir(core));
    again = mf_component_fetch_manifest(unread, &error);
    g_assert_no_error(error);
    g_assert_true(again == manifest);

    /* without a manifest, a version that was removed is not installed anymore */
    removed = mft_new_core(mf_component_get_dir(core));
    g_assert_null(mf_component_fetch_manifest(removed, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_NOT_FOUND);
    g_assert_cmpstr(error->message, ==, TEST_CORE_ID " 13.8 is not installed");
    g_clear_error(&error);

    /* a component the install daemon described was not read from a store, so it has no manifest */
    reported = mft_new_core(NULL);
    g_assert_null(mf_component_get_dir(reported));
    g_assert_null(mf_component_fetch_manifest(reported, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_NOT_FOUND);
    g_assert_cmpstr(error->message, ==, TEST_CORE_ID " 13.8 was not read from a store");
}

/**
 * test_reload:
 */
static void
test_reload(Fixture *f, gconstpointer user_data)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *app_dir = g_build_filename(f->dir, "apps", TEST_APP_ID, NULL);
    g_autoptr(MfComponent) app = mf_store_get_component(f->store, TEST_APP_ID, NULL, &error);
    g_autoptr(MfComponent) cached = NULL;

    g_assert_no_error(error);
    mft_rm_rf(app_dir);

    /* the removed app is still found until the store is loaded again */
    cached = mf_store_get_component(f->store, TEST_APP_ID, NULL, &error);
    g_assert_no_error(error);
    g_assert_true(mf_store_load(f->store, &error));
    g_assert_no_error(error);
    g_assert_null(mf_store_get_component(f->store, TEST_APP_ID, NULL, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_NOT_FOUND);

    /* components handed out before stay usable */
    g_assert_cmpstr(mf_component_get_id(app), ==, TEST_APP_ID);
}

/**
 * main:
 */
int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

#define add_test(name, func) g_test_add("/store-read/" name, Fixture, NULL, mft_setup, func, mft_teardown)
    add_test("get-current", test_get_current);
    add_test("get-version", test_get_version);
    add_test("get-base", test_get_base);
    add_test("base-link-must-name-a-component", test_base_link_must_name_a_component);
    add_test("sdk-app-is-a-runtime", test_sdk_app_is_a_runtime);
    add_test("list", test_list);
    add_test("image-link-must-point-into-store", test_image_link_must_point_into_store);
    add_test("manifest-must-be-sound", test_manifest_must_be_sound);
    add_test("fetch-manifest", test_fetch_manifest);
    add_test("reload", test_reload);
#undef add_test

    return g_test_run();
}
