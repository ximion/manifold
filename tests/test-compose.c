/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <glib/gstdio.h>
#include <manifold.h>

#include "mfr-app.h"
#include "mf-store-private.h"
#include "mft-sandbox.h"
#include "mft-utils.h"

/**
 * mft_write_usual_store:
 *
 * Write the usual store: an application on a kit, on a platform, on a core.
 */
static void
mft_write_usual_store(void)
{
    mft_write_version(MF_STORAGE_DIR,
                      MF_STORE_RUNTIMES_SUBDIR,
                      &(MftTestManifest){.component_id = TEST_CORE_ID, .kind = "core", .version = "13.4"},
                      NULL,
                      NULL);
    mft_write_version(MF_STORAGE_DIR,
                      MF_STORE_RUNTIMES_SUBDIR,
                      &(MftTestManifest){.component_id = TEST_PLATFORM_ID,
                                         .kind = "platform",
                                         .version = "13.4",
                                         .base_id = TEST_CORE_ID},
                      NULL,
                      NULL);
    mft_write_version(MF_STORAGE_DIR,
                      MF_STORE_RUNTIMES_SUBDIR,
                      &(MftTestManifest){.component_id = TEST_PLATFORM_KIT_ID,
                                         .kind = "kit",
                                         .version = "20260114T0238",
                                         .base_id = TEST_PLATFORM_ID},
                      NULL,
                      NULL);
    mft_write_version(MF_STORAGE_DIR,
                      MF_STORE_APPS_SUBDIR,
                      &(MftTestManifest){.component_id = TEST_APP_ID,
                                         .kind = "app",
                                         .version = "0.1.0",
                                         .base_id = TEST_PLATFORM_KIT_ID},
                      NULL,
                      NULL);
}

static void
mft_assert_load_fails(const char *component_id, GQuark domain, int code, const char *message)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(MfrApp) app = mfr_app_load(component_id, &error);

    g_assert_null(app);
    g_assert_error(error, domain, code);
    if (message != NULL)
        g_assert_cmpstr(error->message, ==, message);
}

/* the tests share no state; every one starts afresh */
typedef struct {
    gpointer unused;
} Fixture;

static void
mft_setup(Fixture *fixture, gconstpointer user_data)
{
    mft_clear_store();
    mft_write_usual_store();
}

/*
 * Tests
 */

/**
 * test_load:
 */
static void
test_load(Fixture *fixture, gconstpointer user_data)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(MfrApp) app = mfr_app_load(TEST_APP_ID, &error);
    g_autofree char *app_hash = mft_fake_hash(TEST_APP_ID);
    g_autofree char *core_hash = mft_fake_hash(TEST_CORE_ID);
    const MfrLayer *layer;

    g_assert_no_error(error);
    g_assert_cmpstr(app->component_id, ==, TEST_APP_ID);
    g_assert_cmpstr(app->version, ==, "0.1.0");
    g_assert_cmpstr(app->command, ==, "app");
    g_assert_cmpstr(app->tuple, ==, "x86_64-linux-gnu");

    layer = &app->layers[MFR_LAYER_APP];
    g_assert_cmpint(layer->kind, ==, MFR_LAYER_APP);
    g_assert_cmpstr(layer->component_id, ==, TEST_APP_ID);
    g_assert_cmpstr(layer->verity_hash, ==, app_hash);
    g_assert_cmpuint(layer->data_offset, ==, 20480);
    g_assert_cmpuint(layer->hash_offset, ==, 22528);
    g_assert_cmpstr(app->layers[MFR_LAYER_KIT].component_id, ==, TEST_PLATFORM_KIT_ID);
    g_assert_cmpstr(app->layers[MFR_LAYER_KIT].version, ==, "20260114T0238");
    g_assert_cmpstr(app->layers[MFR_LAYER_PLATFORM].component_id, ==, TEST_PLATFORM_ID);
    g_assert_cmpstr(app->layers[MFR_LAYER_CORE].component_id, ==, TEST_CORE_ID);
    g_assert_cmpstr(app->layers[MFR_LAYER_CORE].verity_hash, ==, core_hash);
}

/**
 * test_without_platform:
 */
static void
test_without_platform(Fixture *fixture, gconstpointer user_data)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(MfrApp) app = NULL;

    /* a kit may sit directly on the core */
    mft_replace_link("../../../runtimes/" TEST_CORE_ID "/current",
                     MF_STORE_RUNTIMES_DIR "/" TEST_PLATFORM_KIT_ID "/20260114T0238-1/base");
    app = mfr_app_load(TEST_APP_ID, &error);

    g_assert_no_error(error);
    g_assert_null(app->layers[MFR_LAYER_PLATFORM].component_id);
    g_assert_cmpstr(app->layers[MFR_LAYER_CORE].component_id, ==, TEST_CORE_ID);
}

/**
 * test_not_installed:
 */
static void
test_not_installed(Fixture *fixture, gconstpointer user_data)
{
    mft_assert_load_fails("org.example.nothing",
                          MF_ERROR,
                          MF_ERROR_NOT_FOUND,
                          "org.example.nothing is not installed");
    /* a runtime is not an application */
    mft_assert_load_fails(TEST_CORE_ID, MF_ERROR, MF_ERROR_NOT_FOUND, NULL);
}

/**
 * test_revoked:
 */
static void
test_revoked(Fixture *fixture, gconstpointer user_data)
{
    mft_write_file(MF_STORE_APPS_DIR "/" TEST_APP_ID "/revoked", "");
    mft_assert_load_fails(TEST_APP_ID, MF_ERROR, MF_ERROR_REVOKED, TEST_APP_ID " was revoked");
}

/**
 * test_missing_base:
 *
 * A base that is not installed is reported as a missing base of the app,
 * and so is a base link that is missing.
 */
static void
test_missing_base(Fixture *fixture, gconstpointer user_data)
{
    mft_replace_link("../../../runtimes/org.example.gone/current",
                     MF_STORE_APPS_DIR "/" TEST_APP_ID "/0.1.0-1/base");
    mft_assert_load_fails(TEST_APP_ID, MF_ERROR, MF_ERROR_MISSING_BASE, NULL);

    g_assert_cmpint(g_unlink(MF_STORE_APPS_DIR "/" TEST_APP_ID "/0.1.0-1/base"), ==, 0);
    mft_assert_load_fails(TEST_APP_ID, MF_ERROR, MF_ERROR_MISSING_BASE, TEST_APP_ID " 0.1.0 has no base");
}

/**
 * test_layers_must_be_in_order:
 */
static void
test_layers_must_be_in_order(Fixture *fixture, gconstpointer user_data)
{
    /* make the kit its own base, so that a kit is below a kit */
    mft_replace_link("../../../runtimes/" TEST_PLATFORM_KIT_ID "/current",
                     MF_STORE_RUNTIMES_DIR "/" TEST_PLATFORM_KIT_ID "/20260114T0238-1/base");
    mft_assert_load_fails(TEST_APP_ID,
                          G_IO_ERROR,
                          G_IO_ERROR_FAILED,
                          "kit " TEST_PLATFORM_KIT_ID " can not be below kit " TEST_PLATFORM_KIT_ID
                          " in the runtime of " TEST_APP_ID);
}

/**
 * test_sdk_is_not_supported_yet:
 *
 * An application can build on an SDK, but we can not run it like that yet.
 */
static void
test_sdk_is_not_supported_yet(Fixture *fixture, gconstpointer user_data)
{
    mft_write_version(MF_STORAGE_DIR,
                      MF_STORE_RUNTIMES_SUBDIR,
                      &(MftTestManifest){.component_id = TEST_PLATFORM_KIT_ID ".sdk",
                                         .kind = "kit",
                                         .version = "20260114T0238",
                                         .sdk = TRUE,
                                         .base_id = TEST_PLATFORM_KIT_ID},
                      NULL,
                      NULL);
    mft_replace_link("../../../runtimes/" TEST_PLATFORM_KIT_ID ".sdk/current",
                     MF_STORE_APPS_DIR "/" TEST_APP_ID "/0.1.0-1/base");
    mft_assert_load_fails(TEST_APP_ID,
                          MF_ERROR,
                          MF_ERROR_NOT_IMPLEMENTED,
                          TEST_APP_ID " builds on the SDK " TEST_PLATFORM_KIT_ID
                                      ".sdk, but running an application on an SDK is not supported yet");
}

int
main(int argc, char **argv)
{
    mft_sandbox_enter();
    g_test_init(&argc, &argv, NULL);

#define add_test(name, func) g_test_add("/run-app/" name, Fixture, NULL, mft_setup, func, NULL)
    add_test("load", test_load);
    add_test("without-platform", test_without_platform);
    add_test("not-installed", test_not_installed);
    add_test("revoked", test_revoked);
    add_test("missing-base", test_missing_base);
    add_test("layers-must-be-in-order", test_layers_must_be_in_order);
    add_test("sdk-is-not-supported-yet", test_sdk_is_not_supported_yet);
#undef add_test

    return g_test_run();
}
