/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mf-manifest.h"

#include <string.h>
#include <systemd/sd-json.h>

#include "mf-error.h"
#include "mf-json-private.h"
#include "mf-sdautoptr-private.h"
#include "mf-utils.h"

/**
 * MfManifest:
 *
 * The manifest of an image contains metadata about its contents:
 * What component it is, what it was built on, what the image file looks
 * like, etc.
 */

constexpr int MANIFEST_FORMAT = 1;

/**
 * mf_checksum_kind_to_string:
 * @kind: a #MfChecksumKind
 *
 * Returns: The name of the checksum kind.
 */
const char *
mf_checksum_kind_to_string(MfChecksumKind kind)
{
    switch (kind) {
    case MF_CHECKSUM_KIND_BLAKE3:
        return "blake3";
    case MF_CHECKSUM_KIND_SHA256:
        return "sha256";
    default:
        return "unknown";
    }
}

/**
 * mf_checksum_kind_from_string:
 * @kind_str: (nullable): the name of a checksum kind
 *
 * Returns: The checksum kind, or %MF_CHECKSUM_KIND_UNKNOWN if the name is not known
 */
MfChecksumKind
mf_checksum_kind_from_string(const char *kind_str)
{
    if (g_strcmp0(kind_str, "blake3") == 0)
        return MF_CHECKSUM_KIND_BLAKE3;
    if (g_strcmp0(kind_str, "sha256") == 0)
        return MF_CHECKSUM_KIND_SHA256;
    return MF_CHECKSUM_KIND_UNKNOWN;
}

typedef struct {
    GBytes *data;

    char *component_id;
    MfComponentKind kind;
    char *version;
    guint64 revision;
    char *architecture;
    gboolean sdk;
    char *identity;
    char *content_hash;

    char *base_id; /* NULL for a core */
    char *base_version;
    guint64 base_revision;
    char *base_identity;
    char *base_content_hash; /* (nullable) */

    char *image_file;
    guint64 image_size;
    char *image_checksums[MF_CHECKSUM_KIND_LAST];
    char *verity_hash;
    guint64 data_offset;
    guint64 data_size;
    guint64 hash_offset;
    guint64 hash_size;

    char *command; /* applications only */
} MfManifestPrivate;

G_DEFINE_TYPE_WITH_PRIVATE(MfManifest, mf_manifest, G_TYPE_OBJECT)

#define GET_PRIVATE(o) ((MfManifestPrivate *) mf_manifest_get_instance_private(o))

static void
mf_manifest_finalize(GObject *object)
{
    MfManifestPrivate *priv = GET_PRIVATE(MF_MANIFEST(object));

    g_clear_pointer(&priv->data, g_bytes_unref);
    g_free(priv->component_id);
    g_free(priv->version);
    g_free(priv->architecture);
    g_free(priv->identity);
    g_free(priv->content_hash);
    g_free(priv->base_id);
    g_free(priv->base_version);
    g_free(priv->base_identity);
    g_free(priv->base_content_hash);
    g_free(priv->image_file);
    for (guint i = 0; i < MF_CHECKSUM_KIND_LAST; i++)
        g_free(priv->image_checksums[i]);
    g_free(priv->verity_hash);
    g_free(priv->command);

    G_OBJECT_CLASS(mf_manifest_parent_class)->finalize(object);
}

static void
mf_manifest_class_init(MfManifestClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = mf_manifest_finalize;
}

static void
mf_manifest_init(MfManifest *manifest)
{
}

/* manifest field accessors, which report every failure as MF_ERROR_INVALID_MANIFEST */
#define FIELD_STRING(object, section, key, error) \
    mf_json_get_string_field(object, section, key, MF_ERROR_INVALID_MANIFEST, error)
#define FIELD_UNSIGNED(object, section, key, ret, error) \
    mf_json_get_unsigned_field(object, section, key, MF_ERROR_INVALID_MANIFEST, ret, error)
#define FIELD_BOOLEAN(object, section, key, ret, error) \
    mf_json_get_boolean_field(object, section, key, MF_ERROR_INVALID_MANIFEST, ret, error)
#define FIELD_HASH(object, section, key, error) \
    mf_json_get_hash_field(object, section, key, MF_ERROR_INVALID_MANIFEST, error)

static void mf_set_invalid_manifest_error(GError **error, const char *format, ...) G_GNUC_PRINTF(2, 3);

/**
 * mf_set_invalid_manifest_error:
 *
 * Set an %MF_ERROR_INVALID_MANIFEST error.
 */
static void
mf_set_invalid_manifest_error(GError **error, const char *format, ...)
{
    g_autofree char *message = NULL;
    va_list args;

    va_start(args, format);
    message = g_strdup_vprintf(format, args);
    va_end(args);
    g_set_error_literal(error, MF_ERROR, MF_ERROR_INVALID_MANIFEST, message);
}

/**
 * mf_manifest_json_get_component_id:
 *
 * Get a field that holds a component-ID, and check that it is one.
 */
static char *
mf_manifest_json_get_component_id(sd_json_variant *object, const char *section, GError **error)
{
    const char *component_id = FIELD_STRING(object, section, "component", error);

    if (component_id == NULL)
        return NULL;
    if (!mf_component_id_is_valid(component_id)) {
        mf_set_invalid_manifest_error(error,
                                      "%s%scomponent identifier is not valid",
                                      section ? section : "",
                                      section ? ": " : "");
        return NULL;
    }
    return g_strdup(component_id);
}

/**
 * mf_manifest_json_get_version:
 *
 * Get a field that holds a version, and check that it is one.
 */
static char *
mf_manifest_json_get_version(sd_json_variant *object, const char *section, GError **error)
{
    const char *version = FIELD_STRING(object, section, "version", error);

    if (version == NULL)
        return NULL;
    if (!mf_version_is_valid(version)) {
        mf_set_invalid_manifest_error(error,
                                      "%s%sversion is not valid",
                                      section ? section : "",
                                      section ? ": " : "");
        return NULL;
    }
    return g_strdup(version);
}

/**
 * mf_manifest_json_parse_base:
 *
 * Parse the base section of a manifest, which names what the image was built
 * on: by its identity, and by its ID, version and revision, which an exact
 * base link names.
 */
static gboolean
mf_manifest_json_parse_base(sd_json_variant *manifest, MfManifestPrivate *priv, GError **error)
{
    sd_json_variant *base = sd_json_variant_by_key(manifest, "base");

    if (base == NULL || sd_json_variant_is_null(base))
        return TRUE;
    if (!sd_json_variant_is_object(base)) {
        mf_set_invalid_manifest_error(error, "base is not an object");
        return FALSE;
    }

    priv->base_identity = g_strdup(FIELD_HASH(base, "base", "identity", error));
    if (priv->base_identity == NULL)
        return FALSE;
    priv->base_id = mf_manifest_json_get_component_id(base, "base", error);
    if (priv->base_id == NULL)
        return FALSE;
    priv->base_version = mf_manifest_json_get_version(base, "base", error);
    if (priv->base_version == NULL)
        return FALSE;
    if (!FIELD_UNSIGNED(base, "base", "revision", &priv->base_revision, error))
        return FALSE;
    priv->base_content_hash = g_strdup(FIELD_HASH(base, "base", "content-hash", error));
    if (priv->base_content_hash == NULL)
        return FALSE;

    return TRUE;
}

/**
 * mf_manifest_json_parse_image:
 *
 * Parse the image section of a manifest, which describes the image file.
 */
static gboolean
mf_manifest_json_parse_image(sd_json_variant *manifest, MfManifestPrivate *priv, GError **error)
{
    sd_json_variant *image = sd_json_variant_by_key(manifest, "image");

    if (image == NULL || !sd_json_variant_is_object(image)) {
        mf_set_invalid_manifest_error(error, "image is missing or not an object");
        return FALSE;
    }

    priv->image_file = g_strdup(FIELD_STRING(image, "image", "file", error));
    if (priv->image_file == NULL)
        return FALSE;
    if (!FIELD_UNSIGNED(image, "image", "size", &priv->image_size, error))
        return FALSE;
    for (MfChecksumKind kind = MF_CHECKSUM_KIND_UNKNOWN + 1; kind < MF_CHECKSUM_KIND_LAST; kind++) {
        priv->image_checksums[kind] = g_strdup(
            FIELD_HASH(image, "image", mf_checksum_kind_to_string(kind), error));
        if (priv->image_checksums[kind] == NULL)
            return FALSE;
    }
    priv->verity_hash = g_strdup(FIELD_HASH(image, "image", "verity-root-hash", error));
    if (priv->verity_hash == NULL)
        return FALSE;
    if (!FIELD_UNSIGNED(image, "image", "data-offset", &priv->data_offset, error) ||
        !FIELD_UNSIGNED(image, "image", "hash-offset", &priv->hash_offset, error))
        return FALSE;
    /* the sizes are a record of the layout, only the offsets are needed */
    if (sd_json_variant_by_key(image, "data-size") != NULL &&
        !FIELD_UNSIGNED(image, "image", "data-size", &priv->data_size, error))
        return FALSE;
    if (sd_json_variant_by_key(image, "hash-size") != NULL &&
        !FIELD_UNSIGNED(image, "image", "hash-size", &priv->hash_size, error))
        return FALSE;
    if (priv->hash_offset <= priv->data_offset) {
        mf_set_invalid_manifest_error(error, "image: the hash partition is placed before the data");
        return FALSE;
    }

    return TRUE;
}

/**
 * mf_manifest_parse_json:
 */
static gboolean
mf_manifest_parse_json(MfManifest *manifest, GError **error)
{
    MfManifestPrivate *priv = GET_PRIVATE(manifest);
    g_autofree char *text = NULL;
    g_autoptr(sd_json_variant) jo = NULL;
    sd_json_variant *app;
    const char *data;
    const char *value;
    guint64 format;
    gsize size;

    /* sd_json_parse needs a terminated string */
    data = g_bytes_get_data(priv->data, &size);
    text = g_strndup(data, size);
    jo = mf_json_parse_object(text, NULL, MF_ERROR_INVALID_MANIFEST, error);
    if (jo == NULL)
        return FALSE;

    if (!FIELD_UNSIGNED(jo, NULL, "format", &format, error))
        return FALSE;
    if (format != (guint64) MANIFEST_FORMAT) {
        mf_set_invalid_manifest_error(error,
                                      "format %" G_GUINT64_FORMAT " is not supported, only %d is",
                                      format,
                                      MANIFEST_FORMAT);
        return FALSE;
    }

    priv->component_id = mf_manifest_json_get_component_id(jo, NULL, error);
    if (priv->component_id == NULL)
        return FALSE;

    value = FIELD_STRING(jo, NULL, "kind", error);
    if (value == NULL)
        return FALSE;
    priv->kind = mf_component_kind_from_string(value);
    if (priv->kind == MF_COMPONENT_KIND_UNKNOWN) {
        mf_set_invalid_manifest_error(error, "kind %s is not known", value);
        return FALSE;
    }

    priv->version = mf_manifest_json_get_version(jo, NULL, error);
    if (priv->version == NULL)
        return FALSE;
    if (!FIELD_UNSIGNED(jo, NULL, "revision", &priv->revision, error))
        return FALSE;
    priv->architecture = g_strdup(FIELD_STRING(jo, NULL, "architecture", error));
    if (priv->architecture == NULL)
        return FALSE;
    if (!FIELD_BOOLEAN(jo, NULL, "sdk", &priv->sdk, error))
        return FALSE;
    priv->identity = g_strdup(FIELD_HASH(jo, NULL, "identity", error));
    if (priv->identity == NULL)
        return FALSE;
    priv->content_hash = g_strdup(FIELD_HASH(jo, NULL, "content-hash", error));
    if (priv->content_hash == NULL)
        return FALSE;

    if (!mf_manifest_json_parse_base(jo, priv, error))
        return FALSE;
    /* only a core has nothing below it, and the SDK of a core builds on that core */
    if (priv->sdk && priv->base_id == NULL) {
        mf_set_invalid_manifest_error(error, "an SDK must name the component it extends");
        return FALSE;
    }
    if (priv->kind != MF_COMPONENT_KIND_CORE && priv->base_id == NULL) {
        mf_set_invalid_manifest_error(error,
                                      "a %s must name the layer it is built on",
                                      mf_component_kind_to_string(priv->kind));
        return FALSE;
    }

    if (!mf_manifest_json_parse_image(jo, priv, error))
        return FALSE;

    app = sd_json_variant_by_key(jo, "app");
    if (app != NULL && !sd_json_variant_is_null(app)) {
        if (!sd_json_variant_is_object(app)) {
            mf_set_invalid_manifest_error(error, "app is not an object");
            return FALSE;
        }
        priv->command = g_strdup(FIELD_STRING(app, "app", "command", error));
        if (priv->command == NULL)
            return FALSE;
        if (strchr(priv->command, '/') != NULL) {
            mf_set_invalid_manifest_error(error, "app: command is not a plain name: %s", priv->command);
            return FALSE;
        }
    } else if (priv->kind == MF_COMPONENT_KIND_APP && !priv->sdk) {
        mf_set_invalid_manifest_error(error, "an app must say what to run");
        return FALSE;
    }

    return TRUE;
}

/**
 * mf_manifest_new_from_bytes:
 * @bytes: the manifest, as JSON
 * @error: a #GError
 *
 * Parse a manifest, and check that it is well-formed.
 *
 * Returns: (transfer full): the manifest, or %NULL with %MF_ERROR_INVALID_MANIFEST
 */
MfManifest *
mf_manifest_new_from_bytes(GBytes *bytes, GError **error)
{
    g_autoptr(MfManifest) manifest = NULL;
    MfManifestPrivate *priv;

    g_return_val_if_fail(bytes != NULL, NULL);

    manifest = g_object_new(MF_TYPE_MANIFEST, NULL);
    priv = GET_PRIVATE(manifest);
    priv->data = g_bytes_ref(bytes);
    if (!mf_manifest_parse_json(manifest, error))
        return NULL;

    return g_steal_pointer(&manifest);
}

/**
 * mf_manifest_new_from_data:
 * @data: (array length=length) (element-type guint8): the manifest, as JSON
 * @length: the length of @data, or -1 if it is NUL terminated
 * @error: a #GError
 *
 * Parse a manifest, like mf_manifest_new_from_bytes().
 *
 * Returns: (transfer full): the manifest, or %NULL with %MF_ERROR_INVALID_MANIFEST
 */
MfManifest *
mf_manifest_new_from_data(const char *data, gssize length, GError **error)
{
    g_autoptr(GBytes) bytes = NULL;

    g_return_val_if_fail(data != NULL, NULL);

    bytes = g_bytes_new(data, length < 0 ? strlen(data) : (gsize) length);
    return mf_manifest_new_from_bytes(bytes, error);
}

/**
 * mf_manifest_new_from_file:
 * @path: a manifest file
 * @error: a #GError
 *
 * Read and parse a manifest, like mf_manifest_new_from_bytes(). The
 * error names the file.
 *
 * Returns: (transfer full): the manifest, or %NULL
 */
MfManifest *
mf_manifest_new_from_file(const char *path, GError **error)
{
    g_autofree char *data = NULL;
    MfManifest *manifest;
    gsize length;

    g_return_val_if_fail(path != NULL, NULL);

    if (!g_file_get_contents(path, &data, &length, error))
        return NULL;
    manifest = mf_manifest_new_from_data(data, (gssize) length, error);
    if (manifest == NULL)
        g_prefix_error(error, "%s: ", path);
    return manifest;
}

/**
 * mf_manifest_get_data:
 * @manifest: an #MfManifest
 *
 * Returns: (transfer none): the plain manifest data that was read
 */
GBytes *
mf_manifest_get_data(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), NULL);
    return GET_PRIVATE(manifest)->data;
}

/**
 * mf_manifest_get_component_id:
 * @manifest: an #MfManifest
 *
 * Returns: the identifier of the component the image belongs to
 */
const char *
mf_manifest_get_component_id(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), NULL);
    return GET_PRIVATE(manifest)->component_id;
}

/**
 * mf_manifest_get_kind:
 * @manifest: an #MfManifest
 *
 * Returns: the kind of the component
 */
MfComponentKind
mf_manifest_get_kind(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), MF_COMPONENT_KIND_UNKNOWN);
    return GET_PRIVATE(manifest)->kind;
}

/**
 * mf_manifest_get_version:
 * @manifest: an #MfManifest
 *
 * Returns: the version of the component
 */
const char *
mf_manifest_get_version(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), NULL);
    return GET_PRIVATE(manifest)->version;
}

/**
 * mf_manifest_get_revision:
 * @manifest: an #MfManifest
 *
 * Returns: the revision of the version, which tells rebuilds of it apart
 */
guint64
mf_manifest_get_revision(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), 0);
    return GET_PRIVATE(manifest)->revision;
}

/**
 * mf_manifest_get_architecture:
 * @manifest: an #MfManifest
 *
 * Returns: the architecture of the image, e.g. `x86_64`
 */
const char *
mf_manifest_get_architecture(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), NULL);
    return GET_PRIVATE(manifest)->architecture;
}

/**
 * mf_manifest_is_sdk:
 * @manifest: an #MfManifest
 *
 * Returns: whether the image is an SDK, a delta with development tools layered over its runtime
 */
gboolean
mf_manifest_is_sdk(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), FALSE);
    return GET_PRIVATE(manifest)->sdk;
}

/**
 * mf_manifest_get_identity:
 * @manifest: an #MfManifest
 *
 * Returns: the identity hash of the image, as hex
 */
const char *
mf_manifest_get_identity(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), NULL);
    return GET_PRIVATE(manifest)->identity;
}

/**
 * mf_manifest_get_content_hash:
 * @manifest: an #MfManifest
 *
 * Returns: the content hash of the image, unique to one exact build, as hex
 */
const char *
mf_manifest_get_content_hash(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), NULL);
    return GET_PRIVATE(manifest)->content_hash;
}

/**
 * mf_manifest_get_base_id:
 * @manifest: an #MfManifest
 *
 * Returns: (nullable): the identifier of the component the image was built on,
 *          or %NULL if the image is a core
 */
const char *
mf_manifest_get_base_id(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), NULL);
    return GET_PRIVATE(manifest)->base_id;
}

/**
 * mf_manifest_get_base_version:
 * @manifest: an #MfManifest
 *
 * Returns: (nullable): the version of the base the image was built on
 */
const char *
mf_manifest_get_base_version(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), NULL);
    return GET_PRIVATE(manifest)->base_version;
}

/**
 * mf_manifest_get_base_revision:
 * @manifest: an #MfManifest
 *
 * Returns: the revision of the base the image was built on, or 0
 */
guint64
mf_manifest_get_base_revision(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), 0);
    return GET_PRIVATE(manifest)->base_revision;
}

/**
 * mf_manifest_get_base_identity:
 * @manifest: an #MfManifest
 *
 * Returns: (nullable): the identity of the base
 */
const char *
mf_manifest_get_base_identity(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), NULL);
    return GET_PRIVATE(manifest)->base_identity;
}

/**
 * mf_manifest_get_base_content_hash:
 * @manifest: an #MfManifest
 *
 * Returns: (nullable): the content hash of the exact base the image was built on
 */
const char *
mf_manifest_get_base_content_hash(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), NULL);
    return GET_PRIVATE(manifest)->base_content_hash;
}

/**
 * mf_manifest_get_image_file:
 * @manifest: an #MfManifest
 *
 * Returns: the name of the image file, beside the manifest
 */
const char *
mf_manifest_get_image_file(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), NULL);
    return GET_PRIVATE(manifest)->image_file;
}

/**
 * mf_manifest_get_image_size:
 * @manifest: an #MfManifest
 *
 * Returns: the size of the image file in bytes, or 0
 */
guint64
mf_manifest_get_image_size(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), 0);
    return GET_PRIVATE(manifest)->image_size;
}

/**
 * mf_manifest_get_image_checksum:
 * @manifest: an #MfManifest
 * @kind: the kind of checksum
 *
 * Returns: the checksum of the image file, as hex
 */
const char *
mf_manifest_get_image_checksum(MfManifest *manifest, MfChecksumKind kind)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), NULL);
    g_return_val_if_fail(kind > MF_CHECKSUM_KIND_UNKNOWN && kind < MF_CHECKSUM_KIND_LAST, NULL);
    return GET_PRIVATE(manifest)->image_checksums[kind];
}

/**
 * mf_manifest_get_verity_hash:
 * @manifest: an #MfManifest
 *
 * Returns: the dm-verity root hash of the image
 */
const char *
mf_manifest_get_verity_hash(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), NULL);
    return GET_PRIVATE(manifest)->verity_hash;
}

/**
 * mf_manifest_get_data_offset:
 * @manifest: an #MfManifest
 *
 * Returns: where the filesystem partition starts in the image file, in bytes
 */
guint64
mf_manifest_get_data_offset(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), 0);
    return GET_PRIVATE(manifest)->data_offset;
}

/**
 * mf_manifest_get_data_size:
 * @manifest: an #MfManifest
 *
 * Returns: the size of the filesystem partition, in bytes, or 0 if unknown
 */
guint64
mf_manifest_get_data_size(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), 0);
    return GET_PRIVATE(manifest)->data_size;
}

/**
 * mf_manifest_get_hash_offset:
 * @manifest: an #MfManifest
 *
 * Returns: where the dm-verity hash partition starts in the image file, in bytes
 */
guint64
mf_manifest_get_hash_offset(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), 0);
    return GET_PRIVATE(manifest)->hash_offset;
}

/**
 * mf_manifest_get_hash_size:
 * @manifest: an #MfManifest
 *
 * Returns: the size of the hash partition, in bytes, or 0 if unknown
 */
guint64
mf_manifest_get_hash_size(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), 0);
    return GET_PRIVATE(manifest)->hash_size;
}

/**
 * mf_manifest_get_command:
 * @manifest: an #MfManifest
 *
 * Returns: (nullable): the program below `/app/bin` an application runs by default
 */
const char *
mf_manifest_get_command(MfManifest *manifest)
{
    g_return_val_if_fail(MF_IS_MANIFEST(manifest), NULL);
    return GET_PRIVATE(manifest)->command;
}
