/*
 * Copyright (C) 2024-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mfr-app.h"

#include <string.h>
#include <libglnx.h>
#include <manifold.h>

#include "mf-store-private.h"

constexpr char REVOKED_NAME[] = "revoked";

static const char *const LAYER_NAMES[MFR_LAYER_COUNT] = {
    [MFR_LAYER_CORE] = "core",
    [MFR_LAYER_PLATFORM] = "platform",
    [MFR_LAYER_KIT] = "kit",
    [MFR_LAYER_APP] = "app",
};

/**
 * mfr_app_append_layers_json:
 * @app: an application
 * @array: (inout): the JSON array to append to
 *
 * Append the layers the application consists of to a JSON array, as the
 * Layer type of the run interface describes them.
 *
 * Returns: 0, or a negative errno
 */
int
mfr_app_append_layers_json(const MfrApp *app, sd_json_variant **array)
{
    for (MfrLayerKind kind = 0; kind < MFR_LAYER_COUNT; kind++) {
        const MfrLayer *layer = &app->layers[kind];

        if (layer->verity_hash == NULL)
            continue;
        int r = sd_json_variant_append_arraybo(array,
                                               SD_JSON_BUILD_PAIR_STRING("kind", LAYER_NAMES[kind]),
                                               SD_JSON_BUILD_PAIR_STRING("componentId", layer->component_id),
                                               SD_JSON_BUILD_PAIR_STRING("version", layer->version),
                                               SD_JSON_BUILD_PAIR_STRING("rootHash", layer->verity_hash));
        if (r < 0)
            return r;
    }
    return 0;
}

/**
 * mfr_runtime_layer_kind_from_component_kind:
 */
static gboolean
mfr_runtime_layer_kind_from_component_kind(MfComponentKind kind, MfrLayerKind *ret_kind)
{
    switch (kind) {
    case MF_COMPONENT_KIND_CORE:
        *ret_kind = MFR_LAYER_CORE;
        return TRUE;
    case MF_COMPONENT_KIND_PLATFORM:
        *ret_kind = MFR_LAYER_PLATFORM;
        return TRUE;
    case MF_COMPONENT_KIND_KIT:
        *ret_kind = MFR_LAYER_KIT;
        return TRUE;
    default:
        return FALSE;
    }
}

/**
 * mfr_fill_layer:
 *
 * Fill out layer information from the storage root and component information.
 * For composition, we need to know where the image is, and where the
 * partitions are in it. The verified manifest holds that info.
 */
static gboolean
mfr_fill_layer(const char *root, MfComponent *component, MfrLayer *layer, GError **error)
{
    g_autoptr(MfManifest) manifest = mf_component_fetch_manifest(component, error);

    if (manifest == NULL)
        return FALSE;
    if (!mf_manifest_has_image(manifest))
        return glnx_throw(error,
                          "The manifest of %s does not say where the partitions of its image are",
                          mf_component_get_id(component));

    layer->component_id = g_strdup(mf_component_get_id(component));
    layer->version = g_strdup(mf_component_get_version(component));
    layer->verity_hash = g_strdup(mf_component_get_verity_hash(component));
    layer->image_path = mf_store_image_path_at(root, layer->verity_hash);
    layer->data_offset = mf_manifest_get_data_offset(manifest);
    layer->hash_offset = mf_manifest_get_hash_offset(manifest);
    return TRUE;
}

/**
 * mfr_app_load_runtime_info:
 *
 * Fill out the runtime layer info below an application, found by following the base links down
 * to the core.
 *
 * We trust the install daemon to only install components whose layers stack up properly, see
 * mf_component_kind_can_build_on(). We only make sure that every layer is of a lower kind than
 * the one above it, so that a broken store can not make us follow base links forever, or fill
 * one layer twice.
 */
static gboolean
mfr_app_load_runtime_info(MfrApp *app, const char *root, MfComponent *app_component, GError **error)
{
    g_autoptr(MfComponent) current = g_object_ref(app_component);
    MfrLayerKind above = MFR_LAYER_APP;

    while (TRUE) {
        g_autoptr(GError) local_error = NULL;
        g_autoptr(MfComponent) next = mf_store_read_base_at(root, current, &local_error);
        MfrLayerKind kind;

        if (next == NULL) {
            /* the application is there, but the layer below it is missing or unreadable */
            if (g_error_matches(local_error, MF_ERROR, MF_ERROR_NOT_FOUND))
                local_error->code = MF_ERROR_MISSING_BASE;
            g_propagate_error(error, g_steal_pointer(&local_error));
            return FALSE;
        }
        /* TODO: Compose the SDK stack, with the SDK of every layer below, for an application
         * that runs on an SDK. */
        if (mf_component_is_sdk(next)) {
            g_set_error(error,
                        MF_ERROR,
                        MF_ERROR_NOT_IMPLEMENTED,
                        "%s builds on the SDK %s, but running an application on an SDK is not supported yet",
                        mf_component_get_id(current),
                        mf_component_get_id(next));
            return FALSE;
        }
        if (!mfr_runtime_layer_kind_from_component_kind(mf_component_get_kind(next), &kind) || kind >= above)
            return glnx_throw(error,
                              "%s %s can not be below %s %s in the runtime of %s",
                              mf_component_kind_to_string(mf_component_get_kind(next)),
                              mf_component_get_id(next),
                              mf_component_kind_to_string(mf_component_get_kind(current)),
                              mf_component_get_id(current),
                              app->component_id);
        if (!mfr_fill_layer(root, next, &app->layers[kind], error))
            return FALSE;

        if (kind == MFR_LAYER_CORE)
            return TRUE;
        above = kind;
        g_clear_object(&current);
        current = g_steal_pointer(&next);
    }
}

/**
 * mfr_build_multiarch_tuple:
 *
 * Manifold names architectures by the CPU part of their multiarch tuple,
 * except 32-bit ARM, which it names by its ABI, as Debian does.
 *
 * Returns: (transfer full): the multiarch tuple of an architecture
 */
static char *
mfr_build_multiarch_tuple(const char *architecture)
{
    if (g_strcmp0(architecture, "armhf") == 0)
        return g_strdup("arm-linux-gnueabihf");
    if (g_strcmp0(architecture, "armel") == 0)
        return g_strdup("arm-linux-gnueabi");
    return g_strconcat(architecture, "-linux-gnu", NULL);
}

G_DEFINE_FINAL_TYPE(MfrApp, mfr_app, G_TYPE_OBJECT)

/**
 * mfr_app_load:
 * @component_id: identifier of an application
 *
 * Read what the store says about the active version of an application.
 */
MfrApp *
mfr_app_load(const char *component_id, GError **error)
{
    g_autoptr(MfrApp) app = NULL;
    g_autoptr(MfComponent) component = NULL;
    g_autoptr(MfManifest) manifest = NULL;
    g_autofree char *revoked_path = NULL;

    g_return_val_if_fail(mf_component_id_is_valid(component_id), NULL);

    /* only what this application needs is read, never the whole store */
    revoked_path = g_build_filename(MF_STORAGE_DIR, MF_STORE_APPS_SUBDIR, component_id, REVOKED_NAME, NULL);
    if (g_file_test(revoked_path, G_FILE_TEST_EXISTS)) {
        g_set_error(error, MF_ERROR, MF_ERROR_REVOKED, "%s was revoked", component_id);
        return NULL;
    }

    component = mf_store_read_component_at(MF_STORAGE_DIR, component_id, NULL, error);
    if (component == NULL)
        return NULL;
    /* a runtime or a plugin is not an application */
    if (mf_component_get_kind(component) != MF_COMPONENT_KIND_APP || mf_component_is_sdk(component)) {
        g_set_error(error, MF_ERROR, MF_ERROR_NOT_FOUND, "%s is not an application", component_id);
        return NULL;
    }

    manifest = mf_component_fetch_manifest(component, error);
    if (manifest == NULL)
        return NULL;
    app = g_object_new(MFR_TYPE_APP, NULL);
    app->component_id = g_strdup(component_id);
    app->version = g_strdup(mf_component_get_version(component));
    app->command = g_strdup(mf_manifest_get_command(manifest));
    if (app->command == NULL)
        return glnx_null_throw(error, "The manifest of %s does not say what to run", component_id);

    app->tuple = mfr_build_multiarch_tuple(mf_manifest_get_architecture(manifest));

    if (!mfr_fill_layer(MF_STORAGE_DIR, component, &app->layers[MFR_LAYER_APP], error))
        return NULL;
    if (!mfr_app_load_runtime_info(app, MF_STORAGE_DIR, component, error))
        return NULL;

    return g_steal_pointer(&app);
}

/**
 * mfr_app_init:
 */
static void
mfr_app_init(MfrApp *app)
{
    for (MfrLayerKind kind = 0; kind < MFR_LAYER_COUNT; kind++)
        app->layers[kind].kind = kind;
}

/**
 * mfr_app_finalize:
 */
static void
mfr_app_finalize(GObject *object)
{
    MfrApp *app = MFR_APP(object);

    for (MfrLayerKind kind = 0; kind < MFR_LAYER_COUNT; kind++) {
        g_free(app->layers[kind].component_id);
        g_free(app->layers[kind].version);
        g_free(app->layers[kind].verity_hash);
        g_free(app->layers[kind].image_path);
    }
    g_free(app->component_id);
    g_free(app->version);
    g_free(app->command);
    g_free(app->tuple);

    G_OBJECT_CLASS(mfr_app_parent_class)->finalize(object);
}

/**
 * mfr_app_class_init:
 */
static void
mfr_app_class_init(MfrAppClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = mfr_app_finalize;
}
