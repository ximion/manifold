/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mf-component-private.h"

#include "mf-error.h"

/**
 * MfComponent:
 *
 * Description of an installed Manifold component.
 */

constexpr size_t COMPONENT_ID_LENGTH_MAX = 255;

struct _MfComponent {
    char *component_id;
    MfComponentKind kind;
    char *version;
    guint64 revision;
    gboolean sdk;
    char *identity;
    char *verity_hash;
    gboolean current;
    gboolean dependency;
    char *origin;
};

G_DEFINE_BOXED_TYPE(MfComponent, mf_component, mf_component_copy, mf_component_free)

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
    if (component_id == NULL)
        return FALSE;

    size_t len = strnlen(component_id, COMPONENT_ID_LENGTH_MAX + 1);
    if (len == 0 || len > COMPONENT_ID_LENGTH_MAX)
        return FALSE;
    if (component_id[0] == '.' || component_id[0] == '-')
        return FALSE;

    for (size_t i = 0; i < len; i++) {
        if (!g_ascii_isalnum(component_id[i]) && strchr("._-", component_id[i]) == NULL)
            return FALSE;
    }

    return TRUE;
}

/**
 * mf_component_copy:
 * @component: an #MfComponent
 *
 * Returns: (transfer full): a copy of the component
 */
MfComponent *
mf_component_copy(const MfComponent *component)
{
    MfComponent *copy;

    g_return_val_if_fail(component != NULL, NULL);

    copy = g_new0(MfComponent, 1);
    copy->component_id = g_strdup(component->component_id);
    copy->kind = component->kind;
    copy->version = g_strdup(component->version);
    copy->revision = component->revision;
    copy->sdk = component->sdk;
    copy->identity = g_strdup(component->identity);
    copy->verity_hash = g_strdup(component->verity_hash);
    copy->current = component->current;
    copy->dependency = component->dependency;
    copy->origin = g_strdup(component->origin);

    return copy;
}

/**
 * mf_component_free:
 * @component: (nullable): an #MfComponent
 */
void
mf_component_free(MfComponent *component)
{
    if (component == NULL)
        return;

    g_free(component->component_id);
    g_free(component->version);
    g_free(component->identity);
    g_free(component->verity_hash);
    g_free(component->origin);
    g_free(component);
}

/**
 * mf_component_get_id:
 * @component: an #MfComponent
 *
 * Returns: the identifier of the component
 */
const char *
mf_component_get_id(const MfComponent *component)
{
    g_return_val_if_fail(component != NULL, NULL);
    return component->component_id;
}

/**
 * mf_component_get_kind:
 * @component: an #MfComponent
 *
 * Returns: the kind of the component
 */
MfComponentKind
mf_component_get_kind(const MfComponent *component)
{
    g_return_val_if_fail(component != NULL, MF_COMPONENT_KIND_UNKNOWN);
    return component->kind;
}

/**
 * mf_component_get_version:
 * @component: an #MfComponent
 *
 * Returns: the version of the component
 */
const char *
mf_component_get_version(const MfComponent *component)
{
    g_return_val_if_fail(component != NULL, NULL);
    return component->version;
}

/**
 * mf_component_get_revision:
 * @component: an #MfComponent
 *
 * Get the revision of the version, which tells rebuilds of it apart.
 * It is usually automatically set by the builder, or by a repository.
 *
 * Returns: the revision number.
 */
guint64
mf_component_get_revision(const MfComponent *component)
{
    g_return_val_if_fail(component != NULL, 0);
    return component->revision;
}

/**
 * mf_component_is_sdk:
 * @component: an #MfComponent
 *
 * Returns: whether this is the SDK image of the component
 */
gboolean
mf_component_is_sdk(const MfComponent *component)
{
    g_return_val_if_fail(component != NULL, FALSE);
    return component->sdk;
}

/**
 * mf_component_get_identity:
 * @component: an #MfComponent
 *
 * Returns: the identity hash of the image, as a hex encoded string
 */
const char *
mf_component_get_identity(const MfComponent *component)
{
    g_return_val_if_fail(component != NULL, NULL);
    return component->identity;
}

/**
 * mf_component_get_verity_hash:
 * @component: an #MfComponent
 *
 * Returns: the dm-verity root hash of the component's image, as a hex encoded string
 */
const char *
mf_component_get_verity_hash(const MfComponent *component)
{
    g_return_val_if_fail(component != NULL, NULL);
    return component->verity_hash;
}

/**
 * mf_component_is_current:
 * @component: an #MfComponent
 *
 * Returns: whether this is the version of the component that is launched
 */
gboolean
mf_component_is_current(const MfComponent *component)
{
    g_return_val_if_fail(component != NULL, FALSE);
    return component->current;
}

/**
 * mf_component_is_dependency:
 * @component: an #MfComponent
 *
 * Returns: whether the component was installed as dependency only.
 */
gboolean
mf_component_is_dependency(const MfComponent *component)
{
    g_return_val_if_fail(component != NULL, FALSE);
    return component->dependency;
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
mf_component_get_origin(const MfComponent *component)
{
    g_return_val_if_fail(component != NULL, NULL);
    return component->origin;
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
        bool dependency;
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
        {"dependency",
         SD_JSON_VARIANT_BOOLEAN, sd_json_dispatch_stdbool,
         offsetof(typeof(fields), dependency),
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
                    "Reply of the Manifold service was not understood: %s",
                    g_strerror(-r));
        return NULL;
    }

    MfComponent *component = g_new0(MfComponent, 1);
    component->component_id = g_strdup(fields.component_id);
    component->kind = mf_component_kind_from_string(fields.kind);
    component->version = g_strdup(fields.version);
    component->sdk = fields.sdk;
    component->identity = g_strdup(fields.identity);
    component->verity_hash = g_strdup(fields.verity_hash);
    component->revision = fields.revision;
    component->current = fields.current;
    component->dependency = fields.dependency;
    component->origin = g_strdup(fields.origin);

    return component;
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
    g_autoptr(GPtrArray) components = g_ptr_array_new_with_free_func((GDestroyNotify) mf_component_free);

    for (size_t i = 0; list != NULL && i < sd_json_variant_elements(list); i++) {
        MfComponent *component = mf_component_new_from_reply(sd_json_variant_by_index(list, i), error);

        if (component == NULL)
            return NULL;
        g_ptr_array_add(components, component);
    }

    return g_steal_pointer(&components);
}
