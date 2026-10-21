/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <glib/gstdio.h>
#include <manifold.h>

#include "mf-store-private.h"
#include "mft-sandbox.h"
#include "mft-utils.h"
#include "mfd-manager.h"
#include "mfd-store-writer.h"

#define IMAGES   MF_STORE_IMAGES_DIR
#define APPS     MF_STORE_APPS_DIR
#define RUNTIMES MF_STORE_RUNTIMES_DIR

/* the store of the sandbox, which every test writes and reads */
static MfStore *store = NULL;
static MfdStoreWriter *writer = NULL;
static MfdManager *manager = NULL;

static void
mft_add_image(const char *hash, gsize size)
{
    g_autofree char *path = g_strdup_printf(IMAGES "/%s.raw", hash);
    g_autofree char *data = g_strnfill(size, 'x');

    g_assert_true(g_file_set_contents(path, data, (gssize) size, NULL));
}

/**
 * mft_manifest_of:
 *
 * Make a manifest that passes the checks of the install daemon. The values
 * of the image section are fake.
 */
static GBytes *
mft_manifest_of(const char *component_id, const char *kind, const char *version, const char *base_id)
{
    char *json = mft_manifest_json(&(MftManifest){.component_id = component_id,
                                                  .kind = kind,
                                                  .version = version,
                                                  .sdk = g_str_has_suffix(component_id, ".sdk"),
                                                  .base_id = base_id});

    return g_bytes_new_take(json, strlen(json));
}

static GBytes *
mft_metainfo_of(const char *component_id)
{
    char *xml = mft_metainfo_xml(component_id);

    return g_bytes_new_take(xml, strlen(xml));
}

/**
 * mft_install_runtime:
 *
 * Install a runtime. With a @base_version, it is pinned to that exact
 * version of its base, otherwise it follows the current version of the base.
 */
static void
mft_install_runtime(const char *component_id,
                    const char *kind,
                    const char *version,
                    const char *hash,
                    const char *base_id,
                    const char *base_version,
                    MfInstallMode mode)
{
    g_autoptr(GBytes) manifest = mft_manifest_of(component_id, kind, version, base_id);
    g_autoptr(GBytes) metainfo = mft_metainfo_of(component_id);
    g_autoptr(GError) error = NULL;

    g_assert_true(mfd_store_writer_install_component(writer,
                                                     MF_STORE_RUNTIMES_SUBDIR,
                                                     component_id,
                                                     version,
                                                     1,
                                                     hash,
                                                     MF_STORE_RUNTIMES_SUBDIR,
                                                     base_id,
                                                     base_version,
                                                     1,
                                                     manifest,
                                                     metainfo,
                                                     mode,
                                                     &error));
    g_assert_no_error(error);
}

static void
mft_install_app_revision(const char *component_id,
                         const char *version,
                         guint64 revision,
                         const char *hash,
                         const char *kit_id)
{
    g_autoptr(GBytes) manifest = mft_manifest_of(component_id, "app", version, kit_id);
    g_autoptr(GBytes) metainfo = mft_metainfo_of(component_id);
    g_autoptr(GError) error = NULL;

    g_assert_true(mfd_store_writer_install_component(writer,
                                                     MF_STORE_APPS_SUBDIR,
                                                     component_id,
                                                     version,
                                                     revision,
                                                     hash,
                                                     MF_STORE_RUNTIMES_SUBDIR,
                                                     kit_id,
                                                     NULL,
                                                     0,
                                                     manifest,
                                                     metainfo,
                                                     MF_INSTALL_MODE_AUTO,
                                                     &error));
    g_assert_no_error(error);
}

static void
mft_install_app(const char *component_id, const char *version, const char *hash, const char *kit_id)
{
    mft_install_app_revision(component_id, version, 1, hash, kit_id);
}

/* every test starts with an empty store; these are the root hashes of the images of the
 * usual setup, which mft_install_usual() installs */
typedef struct {
    char *core_hash;
    char *kit_hash;
    char *app_hash;
} Fixture;

/**
 * mft_install_usual:
 *
 * Install the usual setup: a manually installed core, and a manually installed app with a
 * kit that was installed automatically as its dependency.
 */
static void
mft_install_usual(Fixture *f)
{
    mft_add_image(f->core_hash, 4096);
    mft_add_image(f->kit_hash, 2048);
    mft_add_image(f->app_hash, 1024);
    mft_install_runtime(TEST_CORE_ID, "core", "13.4", f->core_hash, NULL, NULL, MF_INSTALL_MODE_MANUAL);
    mft_install_runtime(TEST_KIT_ID,
                        "kit",
                        "20260112T0238",
                        f->kit_hash,
                        TEST_CORE_ID,
                        NULL,
                        MF_INSTALL_MODE_AUTO);
    mft_install_app(TEST_APP_ID, "0.1.0", f->app_hash, TEST_KIT_ID);
}

static void
mft_assert_link(const char *path, const char *expected_target)
{
    g_autofree char *target = g_file_read_link(path, NULL);

    g_assert_cmpstr(target, ==, expected_target);
}

/**
 * mft_list_components:
 *
 * List the components as the store reader sees them. The daemon uses the
 * reader of libmanifold.
 */
static GPtrArray *
mft_list_components(void)
{
    g_autoptr(GError) error = NULL;
    GPtrArray *components = mf_store_list_components(store, NULL, MF_COMPONENT_KIND_UNKNOWN, &error);

    g_assert_no_error(error);
    return components;
}

static MfComponent *
mft_find_component(GPtrArray *components, const char *component_id, const char *version)
{
    for (guint i = 0; i < components->len; i++) {
        MfComponent *component = g_ptr_array_index(components, i);

        if (g_strcmp0(mf_component_get_id(component), component_id) == 0 &&
            g_strcmp0(mf_component_get_version(component), version) == 0)
            return component;
    }
    return NULL;
}

/**
 * mft_count_versions:
 *
 * Count the versions of a component the store reader sees.
 */
static guint
mft_count_versions(const char *component_id)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GPtrArray)
        components = mf_store_list_components(store, component_id, MF_COMPONENT_KIND_UNKNOWN, &error);

    g_assert_no_error(error);
    return components->len;
}

static void
mft_setup(Fixture *f, gconstpointer user_data)
{
    g_autoptr(GError) error = NULL;

    mft_clear_store();
    /* the directories are gone, so drop what the store has cached */
    mf_store_invalidate(store);
    g_assert_true(mfd_store_writer_ensure_root(writer, &error));
    g_assert_no_error(error);

    f->core_hash = mft_fake_hash("core");
    f->kit_hash = mft_fake_hash("kit");
    f->app_hash = mft_fake_hash("app");
}

static void
mft_teardown(Fixture *f, gconstpointer user_data)
{
    g_free(f->core_hash);
    g_free(f->kit_hash);
    g_free(f->app_hash);
}

/*
 * Tests
 */

/**
 * test_install_runtime:
 */
static void
test_install_runtime(Fixture *f, gconstpointer user_data)
{
    g_autofree char *hash = mft_fake_hash("core");
    g_autofree char *expected_link = g_strdup_printf("../../../images/by-verity/%s.raw", hash);
    g_autofree char *origin = NULL;
    g_autofree char *mode = NULL;
    g_autofree char *manifest = NULL;
    g_autoptr(GBytes) expected_manifest = mft_manifest_of(TEST_CORE_ID, "core", "13.4", NULL);
    g_autoptr(GPtrArray) components = NULL;
    MfComponent *core;

    mft_add_image(hash, 4096);
    mft_install_runtime(TEST_CORE_ID, "core", "13.4", hash, NULL, NULL, MF_INSTALL_MODE_MANUAL);

    /* check the version directory and its contents */
    mft_assert_link(RUNTIMES "/" TEST_CORE_ID "/13.4-1/runtime.raw", expected_link);
    g_assert_false(g_file_test(RUNTIMES "/" TEST_CORE_ID "/13.4-1/base", G_FILE_TEST_EXISTS));
    origin = mft_read_file(RUNTIMES "/" TEST_CORE_ID "/13.4-1/origin");
    g_assert_cmpstr(origin, ==, "local\n");
    mode = mft_read_file(RUNTIMES "/" TEST_CORE_ID "/13.4-1/mode");
    g_assert_cmpstr(mode, ==, "manual\n");
    manifest = mft_read_file(RUNTIMES "/" TEST_CORE_ID "/13.4-1/manifest.json");
    g_assert_cmpstr(manifest, ==, g_bytes_get_data(expected_manifest, NULL));
    mft_assert_link(RUNTIMES "/" TEST_CORE_ID "/current", "13.4-1");

    /* check how the store lists it */
    components = mft_list_components();
    g_assert_cmpuint(components->len, ==, 1);
    core = mft_find_component(components, TEST_CORE_ID, "13.4");
    g_assert_nonnull(core);
    g_assert_cmpint(mf_component_get_kind(core), ==, MF_COMPONENT_KIND_CORE);
    g_assert_cmpuint(mf_component_get_revision(core), ==, 1);
    g_assert_false(mf_component_is_sdk(core));
    g_assert_cmpstr(mf_component_get_verity_hash(core), ==, hash);
    g_assert_true(mf_component_is_current(core));
    g_assert_cmpint(mf_component_get_install_mode(core), ==, MF_INSTALL_MODE_MANUAL);
    g_assert_cmpstr(mf_component_get_origin(core), ==, "local");

    g_assert_true(mf_store_has_image(store, hash));
}

/**
 * test_install_app_with_dependency:
 */
static void
test_install_app_with_dependency(Fixture *f, gconstpointer user_data)
{
    g_autofree char *sdk_hash = mft_fake_hash("sdk");
    g_autofree char *mode = NULL;
    g_autoptr(GPtrArray) components = NULL;
    g_autoptr(MfComponent) kit = NULL;
    g_autoptr(MfComponent) app = NULL;

    mft_install_usual(f);
    /* install an SDK that is pinned to one exact version of the core */
    mft_add_image(sdk_hash, 512);
    mft_install_runtime(TEST_SDK_ID, "core", "13.4", sdk_hash, TEST_CORE_ID, "13.4", MF_INSTALL_MODE_MANUAL);

    /* a base link points to the current version of the base, or to an exact version if pinned */
    mft_assert_link(APPS "/" TEST_APP_ID "/0.1.0-1/base", "../../../runtimes/" TEST_KIT_ID "/current");
    mft_assert_link(RUNTIMES "/" TEST_KIT_ID "/20260112T0238-1/base",
                    "../../../runtimes/" TEST_CORE_ID "/current");
    mft_assert_link(RUNTIMES "/" TEST_SDK_ID "/13.4-1/base", "../../../runtimes/" TEST_CORE_ID "/13.4-1");
    /* every version records its install mode, and an app is always auto */
    mode = mft_read_file(APPS "/" TEST_APP_ID "/0.1.0-1/mode");
    g_assert_cmpstr(mode, ==, "auto\n");
    g_clear_pointer(&mode, g_free);
    mode = mft_read_file(RUNTIMES "/" TEST_KIT_ID "/20260112T0238-1/mode");
    g_assert_cmpstr(mode, ==, "auto\n");

    /* check what is installed, and whether it was installed manually or automatically */
    components = mft_list_components();
    g_assert_cmpuint(components->len, ==, 4);
    kit = mf_store_get_component(store, TEST_KIT_ID, "20260112T0238", NULL);
    g_assert_cmpstr(mf_component_get_verity_hash(kit), ==, f->kit_hash);
    g_assert_cmpint(mf_component_get_install_mode(kit), ==, MF_INSTALL_MODE_AUTO);
    app = mf_store_get_component(store, TEST_APP_ID, "0.1.0", NULL);
    g_assert_cmpstr(mf_component_get_verity_hash(app), ==, f->app_hash);
    g_assert_cmpint(mf_component_get_install_mode(app), ==, MF_INSTALL_MODE_AUTO);
    g_assert_true(mf_component_is_sdk(mft_find_component(components, TEST_SDK_ID, "13.4")));
}

/**
 * test_install_again:
 */
static void
test_install_again(Fixture *f, gconstpointer user_data)
{
    g_autofree char *mode = NULL;
    g_autoptr(MfComponent) kit = NULL;

    mft_install_usual(f);

    /* installing an automatically installed dependency manually switches it to manual mode;
     * installing it automatically again does not switch it back */
    mft_install_runtime(TEST_KIT_ID,
                        "kit",
                        "20260112T0238",
                        f->kit_hash,
                        TEST_CORE_ID,
                        NULL,
                        MF_INSTALL_MODE_MANUAL);
    mode = mft_read_file(RUNTIMES "/" TEST_KIT_ID "/20260112T0238-1/mode");
    g_assert_cmpstr(mode, ==, "manual\n");
    mft_install_runtime(TEST_KIT_ID,
                        "kit",
                        "20260112T0238",
                        f->kit_hash,
                        TEST_CORE_ID,
                        NULL,
                        MF_INSTALL_MODE_AUTO);
    g_clear_pointer(&mode, g_free);
    mode = mft_read_file(RUNTIMES "/" TEST_KIT_ID "/20260112T0238-1/mode");
    g_assert_cmpstr(mode, ==, "manual\n");
    kit = mf_store_get_component(store, TEST_KIT_ID, "20260112T0238", NULL);
    g_assert_cmpint(mf_component_get_install_mode(kit), ==, MF_INSTALL_MODE_MANUAL);

    /* installing an app version that is already installed does not add another version */
    mft_install_app(TEST_APP_ID, "0.1.0", f->app_hash, TEST_KIT_ID);
    g_assert_cmpuint(mft_count_versions(TEST_APP_ID), ==, 1);
}

/**
 * test_resolve_identity:
 */
static void
test_resolve_identity(Fixture *f, gconstpointer user_data)
{
    g_autofree char *core_identity = mft_fake_hash(TEST_CORE_ID);
    g_autofree char *app_identity = mft_fake_hash(TEST_APP_ID);
    g_autofree char *other_identity = mft_fake_hash("nothing");
    g_autofree char *app_image_path = NULL;
    g_autofree char *core2_hash = mft_fake_hash("core2");
    g_autoptr(MfComponent) core = NULL;
    g_autoptr(MfComponent) app = NULL;
    g_autoptr(MfComponent) current = NULL;
    g_autoptr(MfComponent) previous = NULL;
    g_autoptr(GError) error = NULL;

    mft_install_usual(f);

    core = mf_store_find_identity(store, core_identity, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(mf_component_get_verity_hash(core), ==, f->core_hash);
    g_assert_cmpint(mf_component_get_kind(core), ==, MF_COMPONENT_KIND_CORE);
    g_assert_null(mf_store_find_identity(store, other_identity, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_NOT_FOUND);
    g_clear_error(&error);

    /* apps can be found by their identity too, but only while their image exists */
    app = mf_store_find_identity(store, app_identity, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(mf_component_get_verity_hash(app), ==, f->app_hash);
    g_assert_cmpint(mf_component_get_kind(app), ==, MF_COMPONENT_KIND_APP);
    app_image_path = g_strdup_printf(IMAGES "/%s.raw", f->app_hash);
    g_assert_cmpint(g_unlink(app_image_path), ==, 0);
    g_assert_null(mf_store_find_identity(store, app_identity, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_NOT_FOUND);
    g_clear_error(&error);

    /* if several versions have the identity, the current version is found */
    mft_add_image(core2_hash, 4096);
    mft_install_runtime(TEST_CORE_ID, "core", "13.6", core2_hash, NULL, NULL, MF_INSTALL_MODE_MANUAL);
    current = mf_store_find_identity(store, core_identity, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(mf_component_get_version(current), ==, "13.6");
    g_assert_true(mf_component_is_current(current));

    /* if the current version has a different identity, the newest version that still has it is found */
    mft_replace_in_file(RUNTIMES "/" TEST_CORE_ID "/13.6-1/manifest.json", core_identity, other_identity);
    mf_store_invalidate(store);
    previous = mf_store_find_identity(store, core_identity, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(mf_component_get_version(previous), ==, "13.4");
}

/**
 * test_current_is_newest:
 */
static void
test_current_is_newest(Fixture *f, gconstpointer user_data)
{
    g_autofree char *v2_hash = mft_fake_hash("app2");
    g_autofree char *v10_hash = mft_fake_hash("app10");
    g_autoptr(GPtrArray) components = NULL;
    g_autoptr(MfResult) result = NULL;
    g_autoptr(GError) error = NULL;

    mft_install_usual(f);
    mft_add_image(v2_hash, 1024);
    mft_add_image(v10_hash, 1024);

    mft_install_app(TEST_APP_ID, "0.2.0", v2_hash, TEST_KIT_ID);
    mft_assert_link(APPS "/" TEST_APP_ID "/current", "0.2.0-1");
    /* versions are compared by version rules, not as strings: 0.10.0 is newer than 0.2.0,
     * and installing the older 0.3.0 afterwards does not change current */
    mft_install_app(TEST_APP_ID, "0.10.0", v10_hash, TEST_KIT_ID);
    mft_assert_link(APPS "/" TEST_APP_ID "/current", "0.10.0-1");
    mft_install_app(TEST_APP_ID, "0.3.0", f->app_hash, TEST_KIT_ID);
    mft_assert_link(APPS "/" TEST_APP_ID "/current", "0.10.0-1");
    /* a rebuild of the same version has a higher revision, and the highest revision becomes current */
    mft_install_app_revision(TEST_APP_ID, "0.10.0", 22910000, v10_hash, TEST_KIT_ID);
    mft_install_app_revision(TEST_APP_ID, "0.10.0", 22910119, v10_hash, TEST_KIT_ID);
    mft_assert_link(APPS "/" TEST_APP_ID "/current", "0.10.0-22910119");
    g_assert_cmpuint(mft_count_versions(TEST_APP_ID), ==, 6);

    components = mft_list_components();
    g_assert_cmpuint(mf_component_get_revision(mft_find_component(components, TEST_APP_ID, "0.2.0")), ==, 1);
    g_assert_false(mf_component_is_current(mft_find_component(components, TEST_APP_ID, "0.2.0")));

    /* removing the current version, with all of its revisions, makes the next newest version current */
    result = mfd_manager_remove(manager, TEST_APP_ID, "0.10.0", NULL, NULL, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(result);
    mft_assert_link(APPS "/" TEST_APP_ID "/current", "0.3.0-1");
}

/**
 * mft_removal_item:
 *
 * Get the item at @index of a removal plan.
 */
static MfdRemovalItem *
mft_removal_item(MfdRemovalPlan *rmplan, guint index)
{
    return g_ptr_array_index(rmplan->items, index);
}

/**
 * test_plan_removal:
 */
static void
test_plan_removal(Fixture *f, gconstpointer user_data)
{
    g_autofree char *stray_hash = mft_fake_hash("stray");
    g_autoptr(MfdRemovalPlan) app_removal = NULL;
    g_autoptr(MfdRemovalPlan) cleanup = NULL;
    g_autoptr(GError) error = NULL;
    g_auto(MfdErrorDetail) detail = {};

    mft_install_usual(f);
    mft_add_image(stray_hash, 8192);

    /* removing the app also removes the kit that was installed as its dependency, their images,
     * and the stray image nothing uses */
    app_removal = mfd_manager_plan_removal(manager, TEST_APP_ID, NULL, NULL, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(app_removal->items->len, ==, 2);
    g_assert_cmpstr(mft_removal_item(app_removal, 0)->version_dir, ==, APPS "/" TEST_APP_ID "/0.1.0-1");
    g_assert_cmpstr(mft_removal_item(app_removal, 1)->version_dir,
                    ==,
                    RUNTIMES "/" TEST_KIT_ID "/20260112T0238-1");
    g_assert_cmpstr(mf_component_get_id(mft_removal_item(app_removal, 1)->component), ==, TEST_KIT_ID);
    /* the app was asked for, the kit is not needed anymore */
    g_assert_cmpint(mft_removal_item(app_removal, 0)->action, ==, MF_ACTION_REMOVE);
    g_assert_cmpint(mft_removal_item(app_removal, 1)->action, ==, MF_ACTION_CLEANUP);
    g_assert_cmpuint(app_removal->images->len, ==, 3);
    g_assert_cmpuint(app_removal->image_bytes, ==, 1024 + 2048 + 8192);

    /* the core can not be removed: the kit needs it, and it is its only version */
    g_assert_null(mfd_manager_plan_removal(manager, TEST_CORE_ID, "13.4", &detail, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_COMPONENT_IN_USE);
    g_assert_cmpstr(detail.component_id, ==, TEST_CORE_ID);
    g_assert_cmpstr(detail.used_by, ==, TEST_KIT_ID);
    g_clear_error(&error);

    /* a cleanup without a component removes only the images nothing uses */
    cleanup = mfd_manager_plan_removal(manager, NULL, NULL, NULL, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(cleanup->items->len, ==, 0);
    g_assert_cmpuint(cleanup->images->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(cleanup->images, 0), ==, stray_hash);
    g_assert_cmpuint(cleanup->image_bytes, ==, 8192);

    g_assert_null(mfd_manager_plan_removal(manager, "org.example.nothing", NULL, &detail, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_NOT_FOUND);
    g_assert_cmpstr(detail.component_id, ==, "org.example.nothing");
    g_assert_null(detail.used_by);
    g_clear_error(&error);
    g_assert_null(mfd_manager_plan_removal(manager, TEST_APP_ID, "9.9.9", NULL, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_NOT_FOUND);
    g_clear_error(&error);
    /* a version that is not installed is reported as not found, not as in use */
    g_assert_null(mfd_manager_plan_removal(manager, TEST_CORE_ID, "9.9", NULL, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_NOT_FOUND);
}

/**
 * test_exact_base_stays:
 */
static void
test_exact_base_stays(Fixture *f, gconstpointer user_data)
{
    g_autofree char *sdk_hash = mft_fake_hash("sdk");
    g_autofree char *core2_hash = mft_fake_hash("core2");
    g_auto(MfdErrorDetail) detail = {};
    g_autoptr(GError) error = NULL;

    mft_install_usual(f);
    mft_add_image(sdk_hash, 512);
    mft_add_image(core2_hash, 4096);
    mft_install_runtime(TEST_CORE_ID, "core", "13.6", core2_hash, NULL, NULL, MF_INSTALL_MODE_MANUAL);
    mft_install_runtime(TEST_SDK_ID, "core", "13.4", sdk_hash, TEST_CORE_ID, "13.4", MF_INSTALL_MODE_MANUAL);

    /* core 13.4 can not be removed, as the SDK is pinned to it, even though 13.6 is installed too */
    g_assert_null(mfd_manager_plan_removal(manager, TEST_CORE_ID, "13.4", &detail, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_COMPONENT_IN_USE);
    g_assert_cmpstr(detail.used_by, ==, TEST_SDK_ID);
    g_clear_error(&error);

    /* core 13.6 can not be removed either: the kit follows the current version of the core,
     * and removing 13.6 would downgrade the core under the kit */
    g_assert_null(mfd_manager_plan_removal(manager, TEST_CORE_ID, "13.6", &detail, &error));
    g_assert_error(error, MF_ERROR, MF_ERROR_COMPONENT_IN_USE);
    g_assert_cmpstr(detail.used_by, ==, TEST_KIT_ID);
}

/**
 * test_shared_dependency_stays:
 */
static void
test_shared_dependency_stays(Fixture *f, gconstpointer user_data)
{
    g_autofree char *other_hash = mft_fake_hash("other");
    g_autoptr(MfdRemovalPlan) removal = NULL;
    g_autoptr(GError) error = NULL;

    mft_install_usual(f);
    mft_add_image(other_hash, 1024);
    mft_install_app("com.example.other", "1.0", other_hash, TEST_KIT_ID);

    removal = mfd_manager_plan_removal(manager, TEST_APP_ID, NULL, NULL, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(removal->items->len, ==, 1);
    g_assert_cmpuint(removal->images->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(removal->images, 0), ==, f->app_hash);
}

/**
 * test_old_dependency_goes:
 */
static void
test_old_dependency_goes(Fixture *f, gconstpointer user_data)
{
    g_autofree char *kit2_hash = mft_fake_hash("kit2");
    g_autoptr(MfdRemovalPlan) cleanup = NULL;
    g_autoptr(GError) error = NULL;

    mft_install_usual(f);
    mft_add_image(kit2_hash, 2048);
    mft_install_runtime(TEST_KIT_ID,
                        "kit",
                        "20261101T0000",
                        kit2_hash,
                        TEST_CORE_ID,
                        NULL,
                        MF_INSTALL_MODE_AUTO);

    /* the app follows the current version of the kit, so the old kit version is no longer
     * needed, and a cleanup removes it */
    cleanup = mfd_manager_plan_removal(manager, NULL, NULL, NULL, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(cleanup->items->len, ==, 1);
    g_assert_cmpstr(mft_removal_item(cleanup, 0)->version_dir,
                    ==,
                    RUNTIMES "/" TEST_KIT_ID "/20260112T0238-1");
    g_assert_cmpuint(cleanup->images->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(cleanup->images, 0), ==, f->kit_hash);
}

/**
 * test_dependency_chain_removal:
 *
 * What a cleanup removes may be the only thing that kept something else: an
 * old version of an app is removed in one cleanup, together with the kit that
 * only it used.
 */
static void
test_dependency_chain_removal(Fixture *f, gconstpointer user_data)
{
    g_autofree char *old_kit_hash = mft_fake_hash("old-kit");
    g_autofree char *old_app_hash = mft_fake_hash("old-app");
    g_autoptr(MfdRemovalPlan) cleanup = NULL;
    g_autoptr(GError) error = NULL;

    mft_install_usual(f);
    mft_add_image(old_kit_hash, 2048);
    mft_add_image(old_app_hash, 1024);
    mft_install_runtime(TEST_CORE_ID ".0123456789abcdef",
                        "kit",
                        "20251201T0000",
                        old_kit_hash,
                        TEST_CORE_ID,
                        NULL,
                        MF_INSTALL_MODE_AUTO);
    mft_install_app(TEST_APP_ID, "0.0.1", old_app_hash, TEST_CORE_ID ".0123456789abcdef");

    cleanup = mfd_manager_plan_removal(manager, NULL, NULL, NULL, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(cleanup->items->len, ==, 2);
    g_assert_cmpstr(mft_removal_item(cleanup, 0)->version_dir, ==, APPS "/" TEST_APP_ID "/0.0.1-1");
    g_assert_cmpstr(mft_removal_item(cleanup, 1)->version_dir,
                    ==,
                    RUNTIMES "/" TEST_CORE_ID ".0123456789abcdef/20251201T0000-1");
    g_assert_cmpuint(cleanup->images->len, ==, 2);
}

/**
 * test_newest_app_stays:
 *
 * An application is always installed as auto, so a removal or cleanup removes
 * its older versions, unless they are marked manual, but it always keeps the
 * newest version that stays.
 */
static void
test_newest_app_stays(Fixture *f, gconstpointer user_data)
{
    g_autofree char *v2_hash = mft_fake_hash("app2");
    g_autofree char *v3_hash = mft_fake_hash("app3");
    g_autoptr(MfdRemovalPlan) cleanup = NULL;
    g_autoptr(MfdRemovalPlan) removal = NULL;
    g_autoptr(GError) error = NULL;

    mft_install_usual(f);
    mft_add_image(v2_hash, 1024);
    mft_add_image(v3_hash, 1024);
    mft_install_app(TEST_APP_ID, "0.2.0", v2_hash, TEST_KIT_ID);
    mft_install_app(TEST_APP_ID, "0.3.0", v3_hash, TEST_KIT_ID);
    mft_write_file(APPS "/" TEST_APP_ID "/0.1.0-1/mode", "manual\n");
    g_assert_true(mf_store_load(store, &error));
    g_assert_no_error(error);

    cleanup = mfd_manager_plan_removal(manager, NULL, NULL, NULL, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(cleanup->items->len, ==, 1);
    g_assert_cmpstr(mft_removal_item(cleanup, 0)->version_dir, ==, APPS "/" TEST_APP_ID "/0.2.0-1");

    /* without 0.3.0, 0.2.0 is the newest version, and stays */
    removal = mfd_manager_plan_removal(manager, TEST_APP_ID, "0.3.0", NULL, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(removal->items->len, ==, 1);
    g_assert_cmpstr(mft_removal_item(removal, 0)->version_dir, ==, APPS "/" TEST_APP_ID "/0.3.0-1");
}

/**
 * mft_count_removed:
 */
static void
mft_count_removed(const MfProgress *progress, gpointer user_data)
{
    const MfItemProgress *item = mf_progress_get_item(progress);
    guint *counts = user_data; /* components removed on request, and as not needed anymore */

    /* each item is reported when it starts and when it finishes; only count finished ones */
    g_assert_nonnull(item);
    if (mf_item_progress_get_percent(item) < 100)
        return;
    if (mf_item_progress_get_action(item) == MF_ACTION_REMOVE)
        counts[0]++;
    if (mf_item_progress_get_action(item) == MF_ACTION_CLEANUP)
        counts[1]++;
}

/**
 * test_remove:
 */
static void
test_remove(Fixture *f, gconstpointer user_data)
{
    g_autofree char *kit_image = NULL;
    g_autoptr(MfResult) result = NULL;
    g_autoptr(GError) error = NULL;
    guint counts[2] = {0, 0};

    mft_install_usual(f);
    /* leftovers of interrupted installations: in the app's directory, which is removed completely,
     * and in the core's directory, which stays */
    g_assert_cmpint(g_mkdir(APPS "/" TEST_APP_ID "/.tmp-9.9.9-1", 0755), ==, 0);
    g_assert_cmpint(g_mkdir(RUNTIMES "/" TEST_CORE_ID "/.tmp-13.6-1", 0755), ==, 0);

    result = mfd_manager_remove(manager, TEST_APP_ID, NULL, mft_count_removed, counts, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(result);
    /* the app was asked for, the kit is not needed anymore, and images are not reported */
    g_assert_cmpuint(counts[0], ==, 1);
    g_assert_cmpuint(counts[1], ==, 1);
    g_assert_cmpuint(mf_result_get_removed(result)->len, ==, 2);
    g_assert_cmpuint(mf_result_get_removed_images(result)->len, ==, 2);
    g_assert_cmpuint(mf_result_get_bytes_freed(result), ==, 1024 + 2048);

    /* the app and kit directories are removed with their last version; the core stays, but its
     * leftover is removed */
    g_assert_false(g_file_test(APPS "/" TEST_APP_ID, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(RUNTIMES "/" TEST_KIT_ID, G_FILE_TEST_EXISTS));
    g_assert_true(g_file_test(RUNTIMES "/" TEST_CORE_ID "/13.4-1", G_FILE_TEST_IS_DIR));
    g_assert_false(g_file_test(RUNTIMES "/" TEST_CORE_ID "/.tmp-13.6-1", G_FILE_TEST_EXISTS));
    kit_image = g_strdup_printf(IMAGES "/%s.raw", f->kit_hash);
    g_assert_false(g_file_test(kit_image, G_FILE_TEST_EXISTS));
    g_assert_true(mf_store_has_image(store, f->core_hash));
}

/**
 * main:
 */
int
main(int argc, char **argv)
{
    mft_sandbox_enter();
    g_test_init(&argc, &argv, NULL);
    store = mf_store_new();
    writer = mfd_store_writer_new(store);
    manager = mfd_manager_new_for_store(store);

#define add_test(name, func) g_test_add("/store/" name, Fixture, NULL, mft_setup, func, mft_teardown)
    add_test("install-runtime", test_install_runtime);
    add_test("install-app-with-dependency", test_install_app_with_dependency);
    add_test("install-again", test_install_again);
    add_test("resolve-identity", test_resolve_identity);
    add_test("current-is-newest", test_current_is_newest);
    add_test("plan-removal", test_plan_removal);
    add_test("exact-base-stays", test_exact_base_stays);
    add_test("shared-dependency-stays", test_shared_dependency_stays);
    add_test("old-dependency-goes", test_old_dependency_goes);
    add_test("dependency-chain-goes", test_dependency_chain_removal);
    add_test("newest-app-stays", test_newest_app_stays);
    add_test("remove", test_remove);
#undef add_test

    return g_test_run();
}
