/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mf-result-private.h"

#include <glib/gi18n-lib.h>

#include "mf-component-private.h"
#include "mf-error.h"

/**
 * MfResult:
 *
 * Contains results of a change operation.
 */

typedef struct {
    GPtrArray *installed;      /* of MfComponent */
    GPtrArray *removed;        /* of MfComponent */
    GPtrArray *removed_images; /* of char*, root hashes */
    guint64 bytes_added;
    guint64 bytes_freed;
} MfResultPrivate;

G_DEFINE_TYPE_WITH_PRIVATE(MfResult, mf_result, G_TYPE_OBJECT)

#define GET_PRIVATE(o) ((MfResultPrivate *) mf_result_get_instance_private(o))

static void
mf_result_finalize(GObject *object)
{
    MfResultPrivate *priv = GET_PRIVATE(MF_RESULT(object));

    g_clear_pointer(&priv->installed, g_ptr_array_unref);
    g_clear_pointer(&priv->removed, g_ptr_array_unref);
    g_clear_pointer(&priv->removed_images, g_ptr_array_unref);

    G_OBJECT_CLASS(mf_result_parent_class)->finalize(object);
}

static void
mf_result_class_init(MfResultClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = mf_result_finalize;
}

static void
mf_result_init(MfResult *result)
{
    MfResultPrivate *priv = GET_PRIVATE(result);

    priv->installed = g_ptr_array_new_with_free_func(g_object_unref);
    priv->removed = g_ptr_array_new_with_free_func(g_object_unref);
    priv->removed_images = g_ptr_array_new_with_free_func(g_free);
}

/**
 * mf_result_get_installed:
 * @result: an #MfResult
 *
 * Returns: (transfer none) (element-type MfComponent): the components that were installed
 */
GPtrArray *
mf_result_get_installed(MfResult *result)
{
    g_return_val_if_fail(MF_IS_RESULT(result), NULL);
    return GET_PRIVATE(result)->installed;
}

/**
 * mf_result_get_removed:
 * @result: an #MfResult
 *
 * Returns: (transfer none) (element-type MfComponent): the components that were removed
 */
GPtrArray *
mf_result_get_removed(MfResult *result)
{
    g_return_val_if_fail(MF_IS_RESULT(result), NULL);
    return GET_PRIVATE(result)->removed;
}

/**
 * mf_result_get_removed_images:
 * @result: an #MfResult
 *
 * Returns: (transfer none) (element-type utf8): the root hashes of the images that were removed
 */
GPtrArray *
mf_result_get_removed_images(MfResult *result)
{
    g_return_val_if_fail(MF_IS_RESULT(result), NULL);
    return GET_PRIVATE(result)->removed_images;
}

/**
 * mf_result_get_bytes_added:
 * @result: an #MfResult
 *
 * Returns: bytes of disk space the operation took up
 */
guint64
mf_result_get_bytes_added(MfResult *result)
{
    g_return_val_if_fail(MF_IS_RESULT(result), 0);
    return GET_PRIVATE(result)->bytes_added;
}

/**
 * mf_result_get_bytes_freed:
 * @result: an #MfResult
 *
 * Returns: bytes of disk space the operation freed
 */
guint64
mf_result_get_bytes_freed(MfResult *result)
{
    g_return_val_if_fail(MF_IS_RESULT(result), 0);
    return GET_PRIVATE(result)->bytes_freed;
}

/**
 * mf_result_new:
 *
 * Creates a new, empty #MfResult.
 *
 * Returns: (transfer full): a new #MfResult
 */
MfResult *
mf_result_new(void)
{
    return g_object_new(MF_TYPE_RESULT, NULL);
}

/**
 * mf_result_add_installed:
 * @result: an #MfResult
 * @component: (transfer none): a component that was installed
 *
 * Add a component to the ones the operation installed.
 */
void
mf_result_add_installed(MfResult *result, MfComponent *component)
{
    g_return_if_fail(MF_IS_RESULT(result));
    g_return_if_fail(MF_IS_COMPONENT(component));
    g_ptr_array_add(GET_PRIVATE(result)->installed, g_object_ref(component));
}

/**
 * mf_result_add_removed:
 * @result: an #MfResult
 * @component: (transfer none): a component that was removed
 *
 * Add a component to the ones the operation removed.
 */
void
mf_result_add_removed(MfResult *result, MfComponent *component)
{
    g_return_if_fail(MF_IS_RESULT(result));
    g_return_if_fail(MF_IS_COMPONENT(component));
    g_ptr_array_add(GET_PRIVATE(result)->removed, g_object_ref(component));
}

/**
 * mf_result_add_removed_image:
 * @result: an #MfResult
 * @verity_hash: the root hash of an image that was removed
 */
void
mf_result_add_removed_image(MfResult *result, const char *verity_hash)
{
    g_return_if_fail(MF_IS_RESULT(result));
    g_ptr_array_add(GET_PRIVATE(result)->removed_images, g_strdup(verity_hash));
}

/**
 * mf_result_set_bytes_added:
 * @result: an #MfResult
 * @bytes: how much disk space the operation took up
 *
 * Set how many bytes the operation added to the store.
 */
void
mf_result_set_bytes_added(MfResult *result, guint64 bytes)
{
    g_return_if_fail(MF_IS_RESULT(result));
    GET_PRIVATE(result)->bytes_added = bytes;
}

/**
 * mf_result_set_bytes_freed:
 * @result: an #MfResult
 * @bytes: how much disk space the operation freed
 *
 * Set how many bytes the operation freed in the store.
 */
void
mf_result_set_bytes_freed(MfResult *result, guint64 bytes)
{
    g_return_if_fail(MF_IS_RESULT(result));
    GET_PRIVATE(result)->bytes_freed = bytes;
}

/**
 * mf_result_new_from_reply:
 * @reply: the final reply of an operation
 *
 * Returns: (transfer full): The new #MfResult, or %NULL on error.
 */
MfResult *
mf_result_new_from_reply(sd_json_variant *reply, GError **error)
{
    struct {
        uint64_t bytes_added;
        uint64_t bytes_freed;
    } fields = {};
    static const sd_json_dispatch_field fields_table[] = {
        {"bytesAdded",
         _SD_JSON_VARIANT_TYPE_INVALID, sd_json_dispatch_uint64,
         offsetof(typeof(fields), bytes_added),
         0},
        {"bytesFreed",
         _SD_JSON_VARIANT_TYPE_INVALID, sd_json_dispatch_uint64,
         offsetof(typeof(fields), bytes_freed),
         0},
        {},
    };
    g_autoptr(MfResult) result = NULL;
    sd_json_variant *object = sd_json_variant_by_key(reply, "result");
    sd_json_variant *images;
    GPtrArray *components;
    int r;

    r = sd_json_variant_is_object(object)
            ? sd_json_dispatch(object, fields_table, SD_JSON_ALLOW_EXTENSIONS, &fields)
            : -EBADMSG;
    if (r < 0) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_FAILED,
                    _("Reply of the Manifold service was not understood: %s"),
                    g_strerror(-r));
        return NULL;
    }

    result = mf_result_new();
    MfResultPrivate *priv = GET_PRIVATE(result);
    priv->bytes_added = fields.bytes_added;
    priv->bytes_freed = fields.bytes_freed;

    components = mf_component_array_from_reply(sd_json_variant_by_key(object, "installed"), error);
    if (components == NULL)
        return NULL;
    g_ptr_array_unref(priv->installed);
    priv->installed = components;
    components = mf_component_array_from_reply(sd_json_variant_by_key(object, "removed"), error);
    if (components == NULL)
        return NULL;
    g_ptr_array_unref(priv->removed);
    priv->removed = components;

    images = sd_json_variant_by_key(object, "removedImages");
    for (size_t i = 0; images != NULL && i < sd_json_variant_elements(images); i++) {
        sd_json_variant *image = sd_json_variant_by_index(images, i);

        if (!sd_json_variant_is_string(image)) {
            g_set_error_literal(
                error,
                MF_ERROR,
                MF_ERROR_FAILED,
                _("Reply of the Manifold service was not understood: an image is not a string"));
            return NULL;
        }
        g_ptr_array_add(priv->removed_images, g_strdup(sd_json_variant_string(image)));
    }

    return g_steal_pointer(&result);
}
