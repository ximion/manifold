/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mf-component-private.h"

#include <string.h>
#include <glib/gi18n-lib.h>

#include "mf-error.h"
#include "mf-manifest.h"
#include "mf-utils.h"
#include "mf-utils-private.h"

/* the longest an identifier may be, as it is a directory name */
constexpr size_t COMPONENT_ID_LENGTH_MAX = 255;

/**
 * MfComponent:
 *
 * Description of an installed Manifold component.
 */

struct _MfComponent {
    GObject parent_instance;

    char *component_id;
    MfComponentKind kind;
    char *version;
    guint64 revision;
    char *full_version; /* the version and the revision, `<version>-<revision>` */
    gboolean sdk;
    char *identity;
    char *verity_hash;
    gboolean current;
    MfInstallMode install_mode;
    char *origin;
    char *dir;            /* (nullable) the metadata directory for this component version */
    MfManifest *manifest; /* (nullable) kept once it was read */
};

G_DEFINE_FINAL_TYPE(MfComponent, mf_component, G_TYPE_OBJECT)

static void
mf_component_finalize(GObject *object)
{
    MfComponent *component = MF_COMPONENT(object);

    g_free(component->component_id);
    g_free(component->version);
    g_free(component->full_version);
    g_free(component->identity);
    g_free(component->verity_hash);
    g_free(component->origin);
    g_free(component->dir);
    g_clear_object(&component->manifest);

    G_OBJECT_CLASS(mf_component_parent_class)->finalize(object);
}

static void
mf_component_class_init(MfComponentClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = mf_component_finalize;
}

static void
mf_component_init(MfComponent *component)
{
}

/**
 * mf_component_kind_to_string:
 * @kind: an #MfComponentKind
 *
 * Returns: the name of the kind, as the service and manifests use it
 */
const char *
mf_component_kind_to_string(MfComponentKind kind)
{
    switch (kind) {
    case MF_COMPONENT_KIND_CORE:
        return "core";
    case MF_COMPONENT_KIND_PLATFORM:
        return "platform";
    case MF_COMPONENT_KIND_KIT:
        return "kit";
    case MF_COMPONENT_KIND_APP:
        return "app";
    case MF_COMPONENT_KIND_PLUGIN:
        return "plugin";
    default:
        return "unknown";
    }
}

/**
 * mf_component_kind_is_always_auto:
 * @kind: the kind of a component
 * @sdk: whether the component is an SDK
 *
 * Applications and plugins, but not their SDKs, are always installed as
 * %MF_INSTALL_MODE_AUTO, so that their older versions are removed unless
 * they are marked %MF_INSTALL_MODE_MANUAL. We always keep their newest
 * version.
 *
 * Returns: %TRUE for an application or plugin
 */
gboolean
mf_component_kind_is_always_auto(MfComponentKind kind, gboolean sdk)
{
    return !sdk && (kind == MF_COMPONENT_KIND_APP || kind == MF_COMPONENT_KIND_PLUGIN);
}

/**
 * mf_component_kind_can_build_on:
 * @kind: the kind of a component
 * @sdk: whether the component is an SDK
 * @base_kind: the kind of the component it is to build on
 * @base_sdk: whether the component it is to build on is an SDK
 *
 * Check whether a component of one kind can build on a component of another
 * kind. Components stack up in layers: a core has nothing below it, a platform
 * builds on a core, a kit on a platform or a core, an application on a kit, a
 * platform or a core, and a plugin on an application.
 *
 * The layer below can also be an SDK, for example for a development tool that
 * runs on the SDK of its kit. Everything below an SDK is then part of the SDK
 * stack as well, which the builder ensures, so we do not check it here.
 *
 * An SDK can extend the component of its own layer. The SDK of a core, a
 * platform or a kit always does, while the SDK of an application or a plugin
 * can also build on a lower layer: the builder puts the SDK of an application
 * on the SDK of its kit.
 *
 * The install daemon refuses to install a component that breaks these rules,
 * so the compose daemon can rely on them.
 *
 * NOTE: The kinds are declared in the order of their layers, so a component
 * always builds on a kind that is declared before its own kind. Only an SDK
 * builds on its own kind.
 *
 * Returns: %TRUE if a component of @kind can build on a component of @base_kind
 */
gboolean
mf_component_kind_can_build_on(MfComponentKind kind,
                               gboolean sdk,
                               MfComponentKind base_kind,
                               gboolean base_sdk)
{
    if (kind == MF_COMPONENT_KIND_UNKNOWN || base_kind == MF_COMPONENT_KIND_UNKNOWN)
        return FALSE;
    /* an SDK can extend the component of its own layer, which is not an SDK itself */
    if (base_kind == kind)
        return sdk && !base_sdk;

    /* everything else builds on a lower layer, which may be an SDK */
    switch (kind) {
    case MF_COMPONENT_KIND_PLATFORM:
        return !sdk && base_kind == MF_COMPONENT_KIND_CORE;
    case MF_COMPONENT_KIND_KIT:
        return !sdk && (base_kind == MF_COMPONENT_KIND_CORE || base_kind == MF_COMPONENT_KIND_PLATFORM);
    case MF_COMPONENT_KIND_APP:
        return base_kind == MF_COMPONENT_KIND_CORE || base_kind == MF_COMPONENT_KIND_PLATFORM ||
               base_kind == MF_COMPONENT_KIND_KIT;
    case MF_COMPONENT_KIND_PLUGIN:
        return base_kind == MF_COMPONENT_KIND_APP;
    default:
        /* a core has nothing below it, and its SDK extends it */
        return FALSE;
    }
}

/**
 * mf_component_kind_from_string:
 * @kind_str: (nullable): the name of a kind
 *
 * Returns: the kind, or %MF_COMPONENT_KIND_UNKNOWN if the name is not known
 */
MfComponentKind
mf_component_kind_from_string(const char *kind_str)
{
    if (g_strcmp0(kind_str, "core") == 0)
        return MF_COMPONENT_KIND_CORE;
    if (g_strcmp0(kind_str, "platform") == 0)
        return MF_COMPONENT_KIND_PLATFORM;
    if (g_strcmp0(kind_str, "kit") == 0)
        return MF_COMPONENT_KIND_KIT;
    if (g_strcmp0(kind_str, "app") == 0)
        return MF_COMPONENT_KIND_APP;
    if (g_strcmp0(kind_str, "plugin") == 0)
        return MF_COMPONENT_KIND_PLUGIN;
    return MF_COMPONENT_KIND_UNKNOWN;
}

/**
 * mf_install_mode_to_string:
 * @mode: an #MfInstallMode
 *
 * Returns: the name of the install mode
 */
const char *
mf_install_mode_to_string(MfInstallMode mode)
{
    switch (mode) {
    case MF_INSTALL_MODE_MANUAL:
        return "manual";
    case MF_INSTALL_MODE_AUTO:
        return "auto";
    default:
        return "unknown";
    }
}

/**
 * mf_install_mode_from_string:
 * @mode_str: (nullable): the name of a mode
 *
 * Returns: the mode, or %MF_INSTALL_MODE_UNKNOWN if the name is not known
 */
MfInstallMode
mf_install_mode_from_string(const char *mode_str)
{
    if (g_strcmp0(mode_str, "manual") == 0)
        return MF_INSTALL_MODE_MANUAL;
    if (g_strcmp0(mode_str, "auto") == 0)
        return MF_INSTALL_MODE_AUTO;
    return MF_INSTALL_MODE_UNKNOWN;
}

/**
 * mf_component_id_is_valid:
 * @component_id: (nullable): a component identifier
 *
 * Validate a component-ID: It has to have 1 to 255 characters of `[A-Za-z0-9._-]`,
 * not starting with `.` or `-`.
 *
 * Returns: %TRUE if the identifier is valid
 */
gboolean
mf_component_id_is_valid(const char *component_id)
{
    return mf_name_is_valid(component_id, "._-", COMPONENT_ID_LENGTH_MAX);
}

/**
 * mf_component_new:
 * @dir: (nullable): the version directory the component was read from, or
 *   %NULL if it was not read from a store
 *
 * Returns: (transfer full): an installed Manifold component
 */
MfComponent *
mf_component_new(const char *component_id,
                 MfComponentKind kind,
                 const char *version,
                 guint64 revision,
                 gboolean sdk,
                 const char *identity,
                 const char *verity_hash,
                 gboolean current,
                 MfInstallMode install_mode,
                 const char *origin,
                 const char *dir)
{
    MfComponent *component = g_object_new(MF_TYPE_COMPONENT, NULL);

    component->component_id = g_strdup(component_id);
    component->kind = kind;
    component->version = g_strdup(version);
    component->revision = revision;
    component->full_version = mf_build_full_version(version, revision);
    component->sdk = sdk;
    component->identity = g_strdup(identity);
    component->verity_hash = g_strdup(verity_hash);
    component->current = current;
    component->install_mode = install_mode;
    component->origin = g_strdup(origin);
    component->dir = g_strdup(dir);

    return component;
}

/**
 * mf_component_get_id:
 * @component: an #MfComponent
 *
 * Returns: the identifier of the component
 */
const char *
mf_component_get_id(MfComponent *component)
{
    g_return_val_if_fail(MF_IS_COMPONENT(component), NULL);
    return component->component_id;
}

/**
 * mf_component_get_kind:
 * @component: an #MfComponent
 *
 * Returns: the kind of the component
 */
MfComponentKind
mf_component_get_kind(MfComponent *component)
{
    g_return_val_if_fail(MF_IS_COMPONENT(component), MF_COMPONENT_KIND_UNKNOWN);
    return component->kind;
}

/**
 * mf_component_get_version:
 * @component: an #MfComponent
 *
 * Returns: the version of the component
 */
const char *
mf_component_get_version(MfComponent *component)
{
    g_return_val_if_fail(MF_IS_COMPONENT(component), NULL);
    return component->version;
}

/**
 * mf_component_get_revision:
 * @component: an #MfComponent
 *
 * Get the revision of the version, which distinguishes rebuilds of it.
 * It is usually automatically set by the builder, or by a repository.
 *
 * Returns: the revision number.
 */
guint64
mf_component_get_revision(MfComponent *component)
{
    g_return_val_if_fail(MF_IS_COMPONENT(component), 0);
    return component->revision;
}

/**
 * mf_component_get_full_version:
 * @component: an #MfComponent
 *
 * Get the version and the revision of the component as one string,
 * `<version>-<revision>`. This is also the name of the component's
 * directory in the store.
 *
 * Returns: the full version of the component
 */
const char *
mf_component_get_full_version(MfComponent *component)
{
    g_return_val_if_fail(MF_IS_COMPONENT(component), NULL);
    return component->full_version;
}

/**
 * mf_component_is_sdk:
 * @component: an #MfComponent
 *
 * Returns: whether this is the SDK image of the component
 */
gboolean
mf_component_is_sdk(MfComponent *component)
{
    g_return_val_if_fail(MF_IS_COMPONENT(component), FALSE);
    return component->sdk;
}

/**
 * mf_component_get_identity:
 * @component: an #MfComponent
 *
 * Returns: the identity hash of the image, as a hex encoded string
 */
const char *
mf_component_get_identity(MfComponent *component)
{
    g_return_val_if_fail(MF_IS_COMPONENT(component), NULL);
    return component->identity;
}

/**
 * mf_component_get_verity_hash:
 * @component: an #MfComponent
 *
 * Returns: the dm-verity root hash of the component's image, as a hex encoded string
 */
const char *
mf_component_get_verity_hash(MfComponent *component)
{
    g_return_val_if_fail(MF_IS_COMPONENT(component), NULL);
    return component->verity_hash;
}

/**
 * mf_component_is_current:
 * @component: an #MfComponent
 *
 * Returns: whether this is the version of the component that is launched
 */
gboolean
mf_component_is_current(MfComponent *component)
{
    g_return_val_if_fail(MF_IS_COMPONENT(component), FALSE);
    return component->current;
}

/**
 * mf_component_get_install_mode:
 * @component: an #MfComponent
 *
 * Returns: whether the component is kept until it is removed explicitly,
 *   or removed once it is not needed anymore
 */
MfInstallMode
mf_component_get_install_mode(MfComponent *component)
{
    g_return_val_if_fail(MF_IS_COMPONENT(component), MF_INSTALL_MODE_UNKNOWN);
    return component->install_mode;
}

/**
 * mf_component_get_origin:
 * @component: an #MfComponent
 *
 * Get the origin of this component.
 *
 * Returns: where the component came from: the name of a repository, or "local"
 */
const char *
mf_component_get_origin(MfComponent *component)
{
    g_return_val_if_fail(MF_IS_COMPONENT(component), NULL);
    return component->origin;
}

/**
 * mf_component_get_dir:
 * @component: an #MfComponent
 *
 * Get the version directory in the store that the component was read from.
 *
 * Returns: (type filename) (nullable): the directory, or %NULL if the
 *   component was not read from a store, e.g. if the install daemon described it
 */
const char *
mf_component_get_dir(MfComponent *component)
{
    g_return_val_if_fail(MF_IS_COMPONENT(component), NULL);
    return component->dir;
}

/**
 * mf_component_fetch_manifest:
 * @component: an #MfComponent
 * @error: a #GError
 *
 * Get the manifest of the component, which describes it in full. A
 * component read from a store has its manifest, one the install daemon
 * described does not.
 *
 * Returns: (transfer full): the manifest, or %NULL with %MF_ERROR_NOT_FOUND
 */
MfManifest *
mf_component_fetch_manifest(MfComponent *component, GError **error)
{
    g_return_val_if_fail(MF_IS_COMPONENT(component), NULL);

    if (component->manifest == NULL) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_NOT_FOUND,
                    _("%s %s was not read from a store"),
                    component->component_id,
                    component->version);
        return NULL;
    }

    return g_object_ref(component->manifest);
}

/**
 * mf_component_set_manifest:
 * @manifest: the manifest the component was read from
 *
 * Keep the manifest with the component, for mf_component_fetch_manifest()
 * to hand out.
 */
void
mf_component_set_manifest(MfComponent *component, MfManifest *manifest)
{
    g_return_if_fail(MF_IS_COMPONENT(component));
    g_set_object(&component->manifest, manifest);
}

/**
 * mf_component_compare:
 *
 * Order components by kind, from applications down to cores, then by ID,
 * then from the oldest version to the newest.
 */
int
mf_component_compare(gconstpointer a, gconstpointer b)
{
    MfComponent *component_a = (MfComponent *) a;
    MfComponent *component_b = (MfComponent *) b;
    MfComponentKind kind_a = mf_component_get_kind(component_a);
    MfComponentKind kind_b = mf_component_get_kind(component_b);
    int cmp;

    if (kind_a != kind_b)
        return kind_a > kind_b ? -1 : 1;
    cmp = g_strcmp0(mf_component_get_id(component_a), mf_component_get_id(component_b));
    if (cmp != 0)
        return cmp;
    return mf_version_compare_with_rev(mf_component_get_version(component_a),
                                       mf_component_get_revision(component_a),
                                       mf_component_get_version(component_b),
                                       mf_component_get_revision(component_b));
}

/**
 * mf_component_new_from_reply:
 * @variant: a component, as the service describes it
 *
 * Returns: (transfer full): the component, or %NULL if the reply was not
 *   understood
 */
MfComponent *
mf_component_new_from_reply(sd_json_variant *variant, GError **error)
{
    /* NOTE: The strings of the fields point into the reply until they are copied */
    struct {
        const char *component_id;
        const char *kind;
        const char *version;
        uint64_t revision;
        bool sdk;
        const char *identity;
        const char *verity_hash;
        bool current;
        const char *install_mode;
        const char *origin;
    } fields = {};
    static const sd_json_dispatch_field fields_table[] = {
        {"componentId",
         SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string,
         offsetof(typeof(fields), component_id),
         SD_JSON_MANDATORY},
        {"kind",
         SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string,
         offsetof(typeof(fields), kind),
         SD_JSON_MANDATORY},
        {"version",
         SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string,
         offsetof(typeof(fields), version),
         SD_JSON_MANDATORY},
        {"sdk",
         SD_JSON_VARIANT_BOOLEAN, sd_json_dispatch_stdbool,
         offsetof(typeof(fields), sdk),
         SD_JSON_MANDATORY},
        {"identity",
         SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string,
         offsetof(typeof(fields), identity),
         SD_JSON_MANDATORY},
        {"verityHash",
         SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string,
         offsetof(typeof(fields), verity_hash),
         SD_JSON_MANDATORY},
        {"revision",
         _SD_JSON_VARIANT_TYPE_INVALID, sd_json_dispatch_uint64,
         offsetof(typeof(fields), revision),
         SD_JSON_MANDATORY},
        {"current", SD_JSON_VARIANT_BOOLEAN, sd_json_dispatch_stdbool, offsetof(typeof(fields), current), 0},
        {"installMode",
         SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string,
         offsetof(typeof(fields), install_mode),
         0},
        {"origin",
         SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string,
         offsetof(typeof(fields), origin),
         SD_JSON_MANDATORY},
        {},
    };

    int r = sd_json_dispatch(variant, fields_table, SD_JSON_ALLOW_EXTENSIONS, &fields);
    if (r < 0) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_FAILED,
                    _("Reply of the Manifold service was not understood: %s"),
                    g_strerror(-r));
        return NULL;
    }

    return mf_component_new(fields.component_id,
                            mf_component_kind_from_string(fields.kind),
                            fields.version,
                            fields.revision,
                            fields.sdk,
                            fields.identity,
                            fields.verity_hash,
                            fields.current,
                            mf_install_mode_from_string(fields.install_mode),
                            fields.origin,
                            NULL);
}

/**
 * mf_component_array_from_reply:
 * @list: (nullable): an array of components in a reply
 *
 * Returns: (transfer full) (element-type MfComponent): the components, or %NULL on error.
 */
GPtrArray *
mf_component_array_from_reply(sd_json_variant *list, GError **error)
{
    g_autoptr(GPtrArray) components = g_ptr_array_new_with_free_func(g_object_unref);

    for (size_t i = 0; list != NULL && i < sd_json_variant_elements(list); i++) {
        MfComponent *component = mf_component_new_from_reply(sd_json_variant_by_index(list, i), error);

        if (component == NULL)
            return NULL;
        g_ptr_array_add(components, component);
    }

    return g_steal_pointer(&components);
}
