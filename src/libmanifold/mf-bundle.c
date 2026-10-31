/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mf-bundle.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <glib/gi18n-lib.h>
#include <zip.h>
#include <appstream.h>
#include <blake3.h>
#include <systemd/sd-json.h>

#include "mf-error.h"
#include "mf-json-private.h"
#include "mf-sdautoptr-private.h"
#include "mf-utils-private.h"
#include "mf-constants-private.h"

/**
 * MfBundle:
 *
 * A bundle, read and verified.
 *
 * A bundle is a ZIP archive with images, their manifests, an AppStream
 * catalog with a component for every image, an index that records the size
 * and hashes of the manifests and of the files of the catalog, and a
 * signature over the index. Opening a bundle checks every file the index
 * lists against it, by size and by both its hashes, sha256 and blake3,
 * parses the manifests, and sanitizes the catalog, so what reaches the
 * storage is well-formed and describes the components it came with.
 *
 * The images are only verified while they are copied out.
 */

constexpr char INDEX_NAME[] = "index.json";
constexpr char SIGNATURE_NAME[] = "index.json.sig";
constexpr char MANIFEST_SUFFIX[] = ".manifold.json";

constexpr char CATALOG_PREFIX[] = MF_BUNDLE_CATALOG_DIR "/";
constexpr char CATALOG_NAME[] = MF_BUNDLE_CATALOG_DIR "/" MF_BUNDLE_CATALOG_ORIGIN ".xml";
constexpr char CATALOG_ICONS_PREFIX[] = MF_BUNDLE_CATALOG_DIR "/icons/";
constexpr char CATALOG_MEDIA_PREFIX[] = MF_BUNDLE_CATALOG_DIR "/media/";

constexpr int BUNDLE_FORMAT = 1;

/* the index and manifests are small, anything larger is rejected */
constexpr guint64 METADATA_SIZE_MAX = 16 * 1024 * 1024;
/* ... and so is a bundle that makes us read more than this into memory altogether */
constexpr guint64 METADATA_TOTAL_SIZE_MAX = 64 * 1024 * 1024;

constexpr size_t COPY_BUFFER_SIZE = 1024 * 1024;

constexpr size_t FILE_NAME_LENGTH_MAX = 255;
constexpr size_t PATH_LENGTH_MAX = 1024;

/**
 * mf_plain_name_is_valid:
 *
 * Check the first @len bytes of a name for a plain file name: not empty, not
 * hidden, without directories or control characters.
 */
static gboolean
mf_plain_name_is_valid(const char *name, size_t len)
{
    if (len == 0 || len > FILE_NAME_LENGTH_MAX || name[0] == '.')
        return FALSE;
    for (size_t i = 0; i < len; i++) {
        if (name[i] == '/' || name[i] == '\\' || g_ascii_iscntrl(name[i]))
            return FALSE;
    }

    return TRUE;
}

/**
 * mf_file_name_is_valid:
 *
 * The names of the files in a bundle are only used to look entries up and in messages,
 * nothing is ever written to disk under them. We still require plain names (nothing
 * hidden, no control characters), so names are safe to print. Only the catalog has
 * directories: its files have a path of plain names below %MF_BUNDLE_CATALOG_DIR.
 */
static gboolean
mf_file_name_is_valid(const char *name)
{
    const char *part;
    size_t len;

    if (name == NULL)
        return FALSE;
    len = strnlen(name, PATH_LENGTH_MAX + 1);
    if (len > PATH_LENGTH_MAX)
        return FALSE;
    if (!g_str_has_prefix(name, CATALOG_PREFIX))
        return mf_plain_name_is_valid(name, len);

    part = name + strlen(CATALOG_PREFIX);
    for (;;) {
        const char *slash = strchr(part, '/');
        size_t part_len = slash != NULL ? (size_t) (slash - part) : strlen(part);

        if (!mf_plain_name_is_valid(part, part_len))
            return FALSE;
        if (slash == NULL)
            return TRUE;
        part = slash + 1;
    }
}

/**
 * mf_blake3_hex:
 *
 * Returns: the hash of a finished blake3 hasher, as hex
 */
static char *
mf_blake3_hex(const blake3_hasher *hasher)
{
    GString *hex = g_string_sized_new(2 * BLAKE3_OUT_LEN);
    guint8 out[BLAKE3_OUT_LEN];

    blake3_hasher_finalize(hasher, out, sizeof(out));
    for (size_t i = 0; i < sizeof(out); i++)
        g_string_append_printf(hex, "%02x", out[i]);
    return g_string_free(hex, FALSE);
}

/**
 * mf_blake3_of_bytes:
 *
 * Returns: the blake3 hash of the bytes, as hex
 */
static char *
mf_blake3_of_bytes(GBytes *bytes)
{
    blake3_hasher hasher;
    gsize size;
    gconstpointer data = g_bytes_get_data(bytes, &size);

    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, data, size);
    return mf_blake3_hex(&hasher);
}

/**
 * MfBundleImage:
 *
 * An image of a bundle, as its manifest describes it.
 */
struct _MfBundleImage {
    GObject parent_instance;

    char *manifest_name;
    MfManifest *manifest;
    GBytes *catalog;
    GHashTable *icons;   /* size directory and file name -> data */
    guint64 image_index; /* the index of the image file in the archive */
};

G_DEFINE_FINAL_TYPE(MfBundleImage, mf_bundle_image, G_TYPE_OBJECT)

static void
mf_bundle_image_finalize(GObject *object)
{
    MfBundleImage *image = MF_BUNDLE_IMAGE(object);

    g_free(image->manifest_name);
    g_clear_object(&image->manifest);
    g_clear_pointer(&image->catalog, g_bytes_unref);
    g_clear_pointer(&image->icons, g_hash_table_unref);

    G_OBJECT_CLASS(mf_bundle_image_parent_class)->finalize(object);
}

static void
mf_bundle_image_class_init(MfBundleImageClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = mf_bundle_image_finalize;
}

static void
mf_bundle_image_init(MfBundleImage *image)
{
    image->icons = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_bytes_unref);
}

/**
 * mf_bundle_image_get_manifest:
 * @image: an #MfBundleImage
 *
 * Returns: (transfer none): the manifest of the image, as the bundle carries it
 */
MfManifest *
mf_bundle_image_get_manifest(MfBundleImage *image)
{
    g_return_val_if_fail(MF_IS_BUNDLE_IMAGE(image), NULL);
    return image->manifest;
}

/**
 * mf_bundle_image_get_manifest_name:
 * @image: an #MfBundleImage
 *
 * Returns: the name of the manifest file in the bundle
 */
const char *
mf_bundle_image_get_manifest_name(MfBundleImage *image)
{
    g_return_val_if_fail(MF_IS_BUNDLE_IMAGE(image), NULL);
    return image->manifest_name;
}

/**
 * mf_bundle_image_get_catalog:
 * @image: an #MfBundleImage
 *
 * Returns: (transfer none): the AppStream catalog data of the component, as
 *   catalog XML that holds only this component, with the origin %MF_BUNDLE_CATALOG_ORIGIN
 */
GBytes *
mf_bundle_image_get_catalog(MfBundleImage *image)
{
    g_return_val_if_fail(MF_IS_BUNDLE_IMAGE(image), NULL);
    return image->catalog;
}

/**
 * mf_bundle_image_get_icons:
 * @image: an #MfBundleImage
 *
 * Get the cached icons of the component. Each is a JPEG XL file named after
 * the component, in a directory named after its size, like "64x64" or "64x64@2".
 *
 * Returns: (transfer none) (element-type utf8 GBytes): the icons by their
 *   size directory and file name, e.g. "64x64/org.example.App.jxl"
 */
GHashTable *
mf_bundle_image_get_icons(MfBundleImage *image)
{
    g_return_val_if_fail(MF_IS_BUNDLE_IMAGE(image), NULL);
    return image->icons;
}

typedef struct {
    zip_t *zip;
    GPtrArray *images; /* of MfBundleImage */
    GBytes *index_data;
    gboolean is_signed;
} MfBundlePrivate;

G_DEFINE_TYPE_WITH_PRIVATE(MfBundle, mf_bundle, G_TYPE_OBJECT)

#define GET_PRIVATE(o) ((MfBundlePrivate *) mf_bundle_get_instance_private(o))

static void
mf_bundle_finalize(GObject *object)
{
    MfBundlePrivate *priv = GET_PRIVATE(MF_BUNDLE(object));

    if (priv->zip != NULL)
        zip_discard(priv->zip);
    g_clear_pointer(&priv->images, g_ptr_array_unref);
    g_clear_pointer(&priv->index_data, g_bytes_unref);

    G_OBJECT_CLASS(mf_bundle_parent_class)->finalize(object);
}

static void
mf_bundle_class_init(MfBundleClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = mf_bundle_finalize;
}

static void
mf_bundle_init(MfBundle *bundle)
{
    GET_PRIVATE(bundle)->images = g_ptr_array_new_with_free_func(g_object_unref);
}

static void
mf_set_error_from_zip(GError **error, MfError code, zip_error_t *zerr, const char *message)
{
    g_set_error(error, MF_ERROR, code, "%s: %s", message, zip_error_strerror(zerr));
}

/* a file in the archive of a bundle */
typedef struct {
    guint64 index;       /* the index of the file in the archive */
    gboolean referenced; /* the index lists the file, or a manifest names it as its image */
} MfBundleEntry;

/* what opening a bundle keeps track of */
typedef struct {
    MfBundle *bundle;
    GHashTable *entries; /* name -> MfBundleEntry, for every file in the archive */
    guint64 budget;      /* how much metadata may still be read into memory */
} MfBundleReader;

static void
mf_bundle_reader_clear(MfBundleReader *reader)
{
    g_clear_pointer(&reader->entries, g_hash_table_unref);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(MfBundleReader, mf_bundle_reader_clear)

/**
 * mf_bundle_reader_take_reference:
 * @name: the name of a file in the bundle
 * @source: what refers to the file, for messages
 *
 * Record that @source refers to a file of the bundle. Every file in a bundle
 * has to be referred to exactly once: by the index, or by a manifest as its
 * image. The index and its signature count as referred to by themselves.
 * This does not verify the file.
 *
 * Returns: (transfer none): the entry of the file, or %NULL if the bundle
 *   does not have it, or something refers to it already
 */
static MfBundleEntry *
mf_bundle_reader_take_reference(MfBundleReader *reader, const char *name, const char *source, GError **error)
{
    MfBundleEntry *entry = g_hash_table_lookup(reader->entries, name);

    if (entry == NULL) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_VERIFICATION_FAILED,
                    /* TRANSLATORS: what refers to a file, and the name of the file */
                    _("%s refers to %s, which is not in the bundle"),
                    source,
                    name);
        return NULL;
    }
    if (entry->referenced) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_INVALID_BUNDLE,
                    /* TRANSLATORS: what refers to a file, and the name of the file */
                    _("%s refers to %s, which is referred to already"),
                    source,
                    name);
        return NULL;
    }

    entry->referenced = TRUE;
    return entry;
}

/**
 * mf_zip_read_entry:
 * @budget: (inout): how much metadata we may still read, which we lower by the size of the file
 *
 * Read a small file of the bundle into memory. A NUL follows the data,
 * outside of the bytes, so text can be parsed right from them.
 */
static GBytes *
mf_zip_read_entry(zip_t *zip, guint64 index, const char *name, guint64 *budget, GError **error)
{
    g_autofree char *data = NULL;
    zip_stat_t st;
    zip_file_t *file;
    zip_int64_t n;
    gsize total = 0;

    if (zip_stat_index(zip, index, 0, &st) < 0 || !(st.valid & ZIP_STAT_SIZE)) {
        g_set_error(error, MF_ERROR, MF_ERROR_INVALID_BUNDLE, _("Unable to determine size of %s"), name);
        return NULL;
    }
    if (st.size > METADATA_SIZE_MAX) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_INVALID_BUNDLE,
                    g_dngettext(GETTEXT_PACKAGE,
                                "%s is too large (%" PRIu64 " byte)",
                                "%s is too large (%" PRIu64 " bytes)",
                                st.size),
                    name,
                    (guint64) st.size);
        return NULL;
    }
    if (st.size > *budget) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_INVALID_BUNDLE,
                    _("The metadata of the bundle is larger than %" PRIu64 " MiB altogether"),
                    METADATA_TOTAL_SIZE_MAX / (1024 * 1024));
        return NULL;
    }
    *budget -= st.size;

    file = zip_fopen_index(zip, index, 0);
    if (file == NULL) {
        mf_set_error_from_zip(error, MF_ERROR_INVALID_BUNDLE, zip_get_error(zip), name);
        return NULL;
    }

    data = g_malloc(st.size + 1);
    while (total < st.size) {
        n = zip_fread(file, data + total, st.size - total);
        if (n < 0) {
            mf_set_error_from_zip(error, MF_ERROR_INVALID_BUNDLE, zip_file_get_error(file), name);
            zip_fclose(file);
            return NULL;
        }
        if (n == 0)
            break;
        total += (gsize) n;
    }
    zip_fclose(file);

    if (total != st.size) {
        g_set_error(error, MF_ERROR, MF_ERROR_INVALID_BUNDLE, _("%s is shorter than declared"), name);
        return NULL;
    }

    data[total] = '\0';
    return g_bytes_new_take(g_steal_pointer(&data), total);
}

/**
 * mf_zip_hash_entry:
 * @size_max: we stop reading after this many bytes
 *
 * Compute the size and hashes of a file of the bundle, without keeping it in memory.
 */
static gboolean
mf_zip_hash_entry(zip_t *zip,
                  guint64 index,
                  const char *name,
                  guint64 size_max,
                  guint64 *ret_size,
                  char **ret_sha256,
                  char **ret_blake3,
                  GError **error)
{
    g_autofree char *buffer = g_malloc(COPY_BUFFER_SIZE);
    g_autoptr(GChecksum) checksum = g_checksum_new(G_CHECKSUM_SHA256);
    blake3_hasher hasher;
    zip_file_t *file;
    guint64 total = 0;

    file = zip_fopen_index(zip, index, 0);
    if (file == NULL) {
        mf_set_error_from_zip(error, MF_ERROR_INVALID_BUNDLE, zip_get_error(zip), name);
        return FALSE;
    }

    blake3_hasher_init(&hasher);
    while (total <= size_max) {
        zip_int64_t n = zip_fread(file, buffer, COPY_BUFFER_SIZE);

        if (n < 0) {
            mf_set_error_from_zip(error, MF_ERROR_INVALID_BUNDLE, zip_file_get_error(file), name);
            zip_fclose(file);
            return FALSE;
        }
        if (n == 0)
            break;
        total += (guint64) n;
        g_checksum_update(checksum, (const guchar *) buffer, (gsize) n);
        blake3_hasher_update(&hasher, buffer, (gsize) n);
    }
    zip_fclose(file);

    *ret_size = total;
    *ret_sha256 = g_strdup(g_checksum_get_string(checksum));
    *ret_blake3 = mf_blake3_hex(&hasher);
    return TRUE;
}

/**
 * mf_bundle_reader_parse_manifest:
 *
 * Parse a manifest file of the bundle into a #MfBundleImage, and record
 * that the manifest refers to its image file.
 */
static MfBundleImage *
mf_bundle_reader_parse_manifest(MfBundleReader *reader, const char *name, GBytes *data, GError **error)
{
    MfBundlePrivate *priv = GET_PRIVATE(reader->bundle);
    g_autoptr(MfBundleImage) image = NULL;
    MfBundleEntry *image_entry;
    const char *image_name;
    zip_stat_t st;

    image = g_object_new(MF_TYPE_BUNDLE_IMAGE, NULL);
    image->manifest_name = g_strdup(name);
    image->manifest = mf_manifest_new_from_bytes(data, error);
    if (image->manifest == NULL) {
        g_prefix_error(error, "%s: ", name);
        return NULL;
    }

    /* the image has to be in the bundle, and as large as the manifest says */
    image_name = mf_manifest_get_image_file(image->manifest);
    image_entry = mf_bundle_reader_take_reference(reader, image_name, name, error);
    if (image_entry == NULL)
        return NULL;
    image->image_index = image_entry->index;

    if (zip_stat_index(priv->zip, image->image_index, 0, &st) < 0 || !(st.valid & ZIP_STAT_SIZE) ||
        st.size != mf_manifest_get_image_size(image->manifest)) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_VERIFICATION_FAILED,
                    _("Image %s does not have the size its manifest records"),
                    image_name);
        return NULL;
    }

    return g_steal_pointer(&image);
}

/**
 * mf_bundle_reader_verify_indexed_file:
 * @ret_name: (out): the name of the file
 * @ret_data: (out): the contents of the file, or %NULL for screenshot media, which we only hash
 *
 * Check a file that the index lists against its size and hashes in the
 * index. We also record that the index refers to the file.
 */
static gboolean
mf_bundle_reader_verify_indexed_file(MfBundleReader *reader,
                                     sd_json_variant *entry,
                                     char **ret_name,
                                     GBytes **ret_data,
                                     GError **error)
{
    MfBundlePrivate *priv = GET_PRIVATE(reader->bundle);
    MfBundleEntry *archive_entry;
    g_autoptr(GBytes) data = NULL;
    g_autofree char *actual_sha256 = NULL;
    g_autofree char *actual_blake3 = NULL;
    const char *name;
    const char *expected_sha256;
    const char *expected_blake3;
    guint64 size;
    guint64 actual_size;

    if (!sd_json_variant_is_object(entry)) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_INVALID_BUNDLE,
                    /* TRANSLATORS: %s is the name of the index file of the bundle */
                    _("%s: file entry is not an object"),
                    INDEX_NAME);
        return FALSE;
    }

    name = mf_json_get_string_field(entry, INDEX_NAME, "name", MF_ERROR_INVALID_BUNDLE, error);
    if (name == NULL)
        return FALSE;
    if (!mf_file_name_is_valid(name)) {
        /* TRANSLATORS: %s is the name of the index file of the bundle */
        g_set_error(error, MF_ERROR, MF_ERROR_INVALID_BUNDLE, _("%s: a file name is not valid"), INDEX_NAME);
        return FALSE;
    }
    if (!mf_json_get_unsigned_field(entry, INDEX_NAME, "size", MF_ERROR_INVALID_BUNDLE, &size, error))
        return FALSE;
    expected_sha256 = mf_json_get_hash_field(entry, INDEX_NAME, "sha256", MF_ERROR_INVALID_BUNDLE, error);
    if (expected_sha256 == NULL)
        return FALSE;
    expected_blake3 = mf_json_get_hash_field(entry, INDEX_NAME, "blake3", MF_ERROR_INVALID_BUNDLE, error);
    if (expected_blake3 == NULL)
        return FALSE;

    archive_entry = mf_bundle_reader_take_reference(reader, name, INDEX_NAME, error);
    if (archive_entry == NULL)
        return FALSE;

    if (g_str_has_prefix(name, CATALOG_MEDIA_PREFIX)) {
        /* screenshots and videos can be large, so we only hash them */
        if (!mf_zip_hash_entry(priv->zip,
                               archive_entry->index,
                               name,
                               size,
                               &actual_size,
                               &actual_sha256,
                               &actual_blake3,
                               error))
            return FALSE;
    } else {
        data = mf_zip_read_entry(priv->zip, archive_entry->index, name, &reader->budget, error);
        if (data == NULL)
            return FALSE;
        actual_size = g_bytes_get_size(data);
        actual_sha256 = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, data);
        actual_blake3 = mf_blake3_of_bytes(data);
    }
    if (actual_size != size || g_strcmp0(actual_sha256, expected_sha256) != 0 ||
        g_strcmp0(actual_blake3, expected_blake3) != 0) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_VERIFICATION_FAILED,
                    _("%s does not match what the index records"),
                    name);
        return FALSE;
    }

    *ret_name = g_strdup(name);
    *ret_data = g_steal_pointer(&data);
    return TRUE;
}

/**
 * mf_bundle_image_build_ref:
 *
 * Returns: how the catalog names the image, "<architecture>/<component>/<version>"
 */
static char *
mf_bundle_image_build_ref(MfBundleImage *image)
{
    return g_strdup_printf("%s/%s/%s",
                           mf_manifest_get_architecture(image->manifest),
                           mf_manifest_get_component_id(image->manifest),
                           mf_manifest_get_version(image->manifest));
}

/**
 * mf_bytes_are_jxl:
 *
 * Check for a JPEG XL image, as a bare codestream or in its container.
 */
static gboolean
mf_bytes_are_jxl(GBytes *bytes)
{
    static const guint8 codestream[] = {0xff, 0x0a};
    static const guint8 container[] = {0x00, 0x00, 0x00, 0x0c, 'J', 'X', 'L', ' ', '\r', '\n', 0x87, '\n'};
    gsize size;
    const guint8 *data = g_bytes_get_data(bytes, &size);

    return (size >= sizeof(codestream) && memcmp(data, codestream, sizeof(codestream)) == 0) ||
           (size >= sizeof(container) && memcmp(data, container, sizeof(container)) == 0);
}

/**
 * mf_bundle_image_collect_icons:
 * @contents: the files the index lists, by name
 *
 * Collect the cached icons of the component of an image. We only accept JXL
 * files named after the component, so that the icons of all components can
 * share one directory per size once they are installed.
 */
static gboolean
mf_bundle_image_collect_icons(MfBundleImage *image,
                              AsComponent *component,
                              GHashTable *contents,
                              GError **error)
{
    const char *component_id = mf_manifest_get_component_id(image->manifest);
    g_autofree char *icon_name = g_strconcat(component_id, MF_ICON_SUFFIX, NULL);
    GPtrArray *icons = as_component_get_icons(component);

    for (guint i = 0; i < icons->len; i++) {
        AsIcon *icon = g_ptr_array_index(icons, i);
        g_autofree char *size_dir = NULL;
        g_autofree char *path = NULL;
        GBytes *data;

        if (as_icon_get_kind(icon) != AS_ICON_KIND_CACHED)
            continue;
        if (g_strcmp0(as_icon_get_name(icon), icon_name) != 0) {
            g_set_error(error,
                        MF_ERROR,
                        MF_ERROR_INVALID_BUNDLE,
                        /* TRANSLATORS: the component, and the file name its icons must have */
                        _("The icons of %s must be named %s"),
                        component_id,
                        icon_name);
            return FALSE;
        }
        if (as_icon_get_scale(icon) > 1)
            size_dir = g_strdup_printf("%ux%u@%u",
                                       as_icon_get_width(icon),
                                       as_icon_get_height(icon),
                                       as_icon_get_scale(icon));
        else
            size_dir = g_strdup_printf("%ux%u", as_icon_get_width(icon), as_icon_get_height(icon));

        path = g_strconcat(CATALOG_ICONS_PREFIX, size_dir, "/", icon_name, NULL);
        data = g_hash_table_lookup(contents, path);
        if (data == NULL) {
            g_set_error(error,
                        MF_ERROR,
                        MF_ERROR_INVALID_BUNDLE,
                        /* TRANSLATORS: the icon file, and the component it belongs to */
                        _("The icon %s of %s is not in the bundle"),
                        path,
                        component_id);
            return FALSE;
        }
        if (!mf_bytes_are_jxl(data)) {
            g_set_error(error, MF_ERROR, MF_ERROR_INVALID_BUNDLE, _("%s is not a JPEG XL image"), path);
            return FALSE;
        }
        g_hash_table_insert(image->icons, g_strconcat(size_dir, "/", icon_name, NULL), g_bytes_ref(data));
    }

    return TRUE;
}

/**
 * mf_bundle_image_make_catalog:
 *
 * Write the component of an image as catalog XML of its own, with the
 * origin of bundles, so that we only store catalog data we could read.
 */
static GBytes *
mf_bundle_image_make_catalog(MfBundleImage *image, AsComponent *component, GError **error)
{
    g_autoptr(AsMetadata) metadata = as_metadata_new();
    g_autoptr(GError) local_error = NULL;
    char *xml;

    as_metadata_set_locale(metadata, "ALL");
    as_metadata_set_format_style(metadata, AS_FORMAT_STYLE_CATALOG);
    as_metadata_set_origin(metadata, MF_BUNDLE_CATALOG_ORIGIN);
    as_metadata_add_component(metadata, component);
    xml = as_metadata_components_to_catalog(metadata, AS_FORMAT_KIND_XML, &local_error);
    if (xml == NULL) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_INVALID_BUNDLE,
                    /* TRANSLATORS: the component, and why its catalog data cannot be written */
                    _("The catalog data of %s cannot be written: %s"),
                    mf_manifest_get_component_id(image->manifest),
                    local_error->message);
        return NULL;
    }

    return g_bytes_new_take(xml, strlen(xml));
}

/**
 * mf_bundle_reader_read_catalog:
 * @contents: the files the index lists, by name
 *
 * Parse the catalog of the bundle, and give every image its component and
 * icons. A component names its image with the ID of its Manifold bundle,
 * and every image has exactly one component.
 */
static gboolean
mf_bundle_reader_read_catalog(MfBundleReader *reader, GHashTable *contents, GError **error)
{
    MfBundlePrivate *priv = GET_PRIVATE(reader->bundle);
    GBytes *catalog = g_hash_table_lookup(contents, CATALOG_NAME);
    g_autoptr(AsMetadata) metadata = as_metadata_new();
    g_autoptr(GError) local_error = NULL;
    AsComponentBox *components;

    if (catalog == NULL) {
        g_set_error(error, MF_ERROR, MF_ERROR_INVALID_BUNDLE, _("The bundle has no %s"), CATALOG_NAME);
        return FALSE;
    }
    as_metadata_set_locale(metadata, "ALL");
    as_metadata_set_format_style(metadata, AS_FORMAT_STYLE_CATALOG);
    if (!as_metadata_parse_bytes(metadata, catalog, AS_FORMAT_KIND_XML, &local_error)) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_INVALID_BUNDLE,
                    _("%s is not valid: %s"),
                    CATALOG_NAME,
                    local_error->message);
        return FALSE;
    }

    components = as_metadata_get_components(metadata);
    for (guint i = 0; i < as_component_box_len(components); i++) {
        AsComponent *component = as_component_box_index(components, i);
        AsBundle *bundle = as_component_get_bundle(component, AS_BUNDLE_KIND_MANIFOLD);
        const char *ref = bundle != NULL ? as_bundle_get_id(bundle) : NULL;
        const char *id = as_component_get_id(component);
        MfBundleImage *image = NULL;

        for (guint j = 0; ref != NULL && image == NULL && j < priv->images->len; j++) {
            MfBundleImage *candidate = g_ptr_array_index(priv->images, j);
            g_autofree char *candidate_ref = mf_bundle_image_build_ref(candidate);

            if (g_str_equal(ref, candidate_ref))
                image = candidate;
        }
        if (image == NULL) {
            g_set_error(error,
                        MF_ERROR,
                        MF_ERROR_INVALID_BUNDLE,
                        /* TRANSLATORS: the catalog file, and a component in it */
                        _("%s describes %s, which is not in the bundle"),
                        CATALOG_NAME,
                        ref != NULL ? ref : (id != NULL ? id : "?"));
            return FALSE;
        }
        if (image->catalog != NULL) {
            g_set_error(error,
                        MF_ERROR,
                        MF_ERROR_INVALID_BUNDLE,
                        /* TRANSLATORS: the catalog file, and the image it describes twice */
                        _("%s describes %s twice"),
                        CATALOG_NAME,
                        ref);
            return FALSE;
        }
        if (g_strcmp0(id, mf_manifest_get_component_id(image->manifest)) != 0) {
            g_set_error(error,
                        MF_ERROR,
                        MF_ERROR_INVALID_BUNDLE,
                        /* TRANSLATORS: the catalog file, the component it describes, and the expected one */
                        _("%s describes %s, not %s"),
                        CATALOG_NAME,
                        id != NULL ? id : "?",
                        mf_manifest_get_component_id(image->manifest));
            return FALSE;
        }

        if (!mf_bundle_image_collect_icons(image, component, contents, error))
            return FALSE;
        image->catalog = mf_bundle_image_make_catalog(image, component, error);
        if (image->catalog == NULL)
            return FALSE;
    }

    for (guint i = 0; i < priv->images->len; i++) {
        MfBundleImage *image = g_ptr_array_index(priv->images, i);

        if (image->catalog == NULL) {
            g_autofree char *ref = mf_bundle_image_build_ref(image);

            g_set_error(error,
                        MF_ERROR,
                        MF_ERROR_INVALID_BUNDLE,
                        /* TRANSLATORS: the catalog file, and an image it has no component for */
                        _("%s does not describe %s"),
                        CATALOG_NAME,
                        ref);
            return FALSE;
        }
    }

    return TRUE;
}

/**
 * mf_bundle_reader_read_index:
 *
 * Verify every file the index lists, parse the manifests among them, and
 * give every image its catalog data.
 */
static gboolean
mf_bundle_reader_read_index(MfBundleReader *reader, GError **error)
{
    MfBundlePrivate *priv = GET_PRIVATE(reader->bundle);
    g_autoptr(GHashTable) contents = NULL;
    g_autoptr(sd_json_variant) index = NULL;
    MfBundleEntry *index_entry;
    sd_json_variant *files;
    guint64 format;

    contents = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_bytes_unref);
    index_entry = g_hash_table_lookup(reader->entries, INDEX_NAME);
    if (index_entry == NULL) {
        /* TRANSLATORS: %s is the name of the index file of the bundle */
        g_set_error(error, MF_ERROR, MF_ERROR_INVALID_BUNDLE, _("The bundle has no %s"), INDEX_NAME);
        return FALSE;
    }
    /* the index cannot list itself */
    index_entry->referenced = TRUE;

    priv->index_data = mf_zip_read_entry(priv->zip, index_entry->index, INDEX_NAME, &reader->budget, error);
    if (priv->index_data == NULL)
        return FALSE;
    index = mf_json_parse_object(g_bytes_get_data(priv->index_data, NULL),
                                 INDEX_NAME,
                                 MF_ERROR_INVALID_BUNDLE,
                                 error);
    if (index == NULL)
        return FALSE;

    if (!mf_json_get_unsigned_field(index, INDEX_NAME, "format", MF_ERROR_INVALID_BUNDLE, &format, error))
        return FALSE;
    if (format != (guint64) BUNDLE_FORMAT) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_INVALID_BUNDLE,
                    /* TRANSLATORS: the index file of the bundle, its format, and the supported one */
                    _("%s has format %" PRIu64 ", only %d is supported"),
                    INDEX_NAME,
                    format,
                    BUNDLE_FORMAT);
        return FALSE;
    }

    files = sd_json_variant_by_key(index, "files");
    if (files == NULL || !sd_json_variant_is_array(files)) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_INVALID_BUNDLE,
                    /* TRANSLATORS: %s is the index file of the bundle, "files" is not translated */
                    _("%s: files is missing or not an array"),
                    INDEX_NAME);
        return FALSE;
    }

    for (size_t i = 0; i < sd_json_variant_elements(files); i++) {
        g_autofree char *name = NULL;
        g_autoptr(GBytes) data = NULL;

        if (!mf_bundle_reader_verify_indexed_file(reader,
                                                  sd_json_variant_by_index(files, i),
                                                  &name,
                                                  &data,
                                                  error))
            return FALSE;

        if (!g_str_has_prefix(name, CATALOG_PREFIX) && g_str_has_suffix(name, MANIFEST_SUFFIX)) {
            MfBundleImage *image = mf_bundle_reader_parse_manifest(reader, name, data, error);

            if (image == NULL)
                return FALSE;
            g_ptr_array_add(priv->images, image);
        } else if (data != NULL) {
            g_hash_table_insert(contents, g_steal_pointer(&name), g_steal_pointer(&data));
        }
    }

    /* the caller reports a bundle without images */
    if (priv->images->len == 0)
        return TRUE;
    return mf_bundle_reader_read_catalog(reader, contents, error);
}

/**
 * mf_bundle_check_kinds:
 *
 * An ID names exactly one kind of component. This function checks that the
 * images of the bundle that have the same ID are the same kind of component,
 * and that either all of them are SDKs or none.
 */
static gboolean
mf_bundle_check_kinds(MfBundle *bundle, GError **error)
{
    GPtrArray *images = GET_PRIVATE(bundle)->images;

    for (guint i = 0; i < images->len; i++) {
        MfManifest *manifest = ((MfBundleImage *) g_ptr_array_index(images, i))->manifest;

        for (guint j = 0; j < i; j++) {
            MfManifest *other = ((MfBundleImage *) g_ptr_array_index(images, j))->manifest;

            if (g_str_equal(mf_manifest_get_component_id(manifest), mf_manifest_get_component_id(other)) &&
                (mf_manifest_get_kind(manifest) != mf_manifest_get_kind(other) ||
                 mf_manifest_is_sdk(manifest) != mf_manifest_is_sdk(other))) {
                g_set_error(error,
                            MF_ERROR,
                            MF_ERROR_INVALID_BUNDLE,
                            _("The bundle contains %s as two different kinds of component"),
                            mf_manifest_get_component_id(manifest));
                return FALSE;
            }
        }
    }

    return TRUE;
}

/**
 * mf_bundle_new_from_fd:
 * @fd: file descriptor of the bundle, opened for reading
 * @error: a #GError
 *
 * Open a bundle, and verify everything in it but the images. We do not take
 * over @fd: the caller still has to close it.
 *
 * Returns: (transfer full): the bundle, or %NULL on failure
 */
MfBundle *
mf_bundle_new_from_fd(int fd, GError **error)
{
    g_autoptr(MfBundle) bundle = NULL;
    g_auto(MfBundleReader) reader = {.budget = METADATA_TOTAL_SIZE_MAX};
    MfBundlePrivate *priv;
    MfBundleEntry *signature_entry;
    GHashTableIter iter;
    gpointer entry_name;
    gpointer entry_ptr;
    zip_int64_t n_entries;
    zip_source_t *source;
    zip_error_t zerr;
    FILE *file;
    int fd_copy;

    g_return_val_if_fail(fd >= 0, NULL);

    /* we do not own the descriptor, so the archive gets a copy, which it closes. zip_fdopen()
     * would make the copy without close-on-exec, and children would inherit it. */
    fd_copy = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    if (fd_copy < 0) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_FAILED,
                    _("Unable to duplicate file descriptor: %s"),
                    g_strerror(errno));
        return NULL;
    }
    file = fdopen(fd_copy, "rb");
    if (file == NULL) {
        g_set_error(error, MF_ERROR, MF_ERROR_FAILED, _("Unable to read the bundle: %s"), g_strerror(errno));
        close(fd_copy);
        return NULL;
    }

    bundle = g_object_new(MF_TYPE_BUNDLE, NULL);
    priv = GET_PRIVATE(bundle);
    reader.bundle = bundle;

    zip_error_init(&zerr);
    /* a length of -1 reads to the end */
    source = zip_source_filep_create(file, 0, -1, &zerr);
    if (source == NULL)
        fclose(file);
    else if ((priv->zip = zip_open_from_source(source, ZIP_RDONLY, &zerr)) == NULL)
        zip_source_free(source);
    if (priv->zip == NULL) {
        mf_set_error_from_zip(error, MF_ERROR_INVALID_BUNDLE, &zerr, _("Not a bundle"));
        zip_error_fini(&zerr);
        return NULL;
    }
    zip_error_fini(&zerr);

    /* what is in the archive */
    reader.entries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    n_entries = zip_get_num_entries(priv->zip, 0);
    for (zip_int64_t i = 0; i < n_entries; i++) {
        const char *name = zip_get_name(priv->zip, (zip_uint64_t) i, 0);
        MfBundleEntry *entry;

        if (!mf_file_name_is_valid(name)) {
            g_set_error_literal(error,
                                MF_ERROR,
                                MF_ERROR_INVALID_BUNDLE,
                                _("The bundle contains a file with an invalid name"));
            return NULL;
        }
        if (g_hash_table_contains(reader.entries, name)) {
            g_set_error(error, MF_ERROR, MF_ERROR_INVALID_BUNDLE, _("The bundle contains %s twice"), name);
            return NULL;
        }
        entry = g_new0(MfBundleEntry, 1);
        entry->index = (guint64) i;
        g_hash_table_insert(reader.entries, g_strdup(name), entry);
    }

    /* the index cannot list its own signature, which is made over the index */
    signature_entry = g_hash_table_lookup(reader.entries, SIGNATURE_NAME);
    if (signature_entry != NULL)
        signature_entry->referenced = TRUE;
    priv->is_signed = signature_entry != NULL;

    if (!mf_bundle_reader_read_index(&reader, error))
        return NULL;

    if (priv->images->len == 0) {
        g_set_error_literal(error, MF_ERROR, MF_ERROR_INVALID_BUNDLE, _("The bundle contains no images"));
        return NULL;
    }
    if (!mf_bundle_check_kinds(bundle, error))
        return NULL;

    /* nothing may be in the bundle that nothing refers to */
    g_hash_table_iter_init(&iter, reader.entries);
    while (g_hash_table_iter_next(&iter, &entry_name, &entry_ptr)) {
        const MfBundleEntry *entry = entry_ptr;

        if (!entry->referenced) {
            g_set_error(error,
                        MF_ERROR,
                        MF_ERROR_INVALID_BUNDLE,
                        _("The bundle contains %s, which its index does not list"),
                        (const char *) entry_name);
            return NULL;
        }
    }

    return g_steal_pointer(&bundle);
}

/**
 * mf_bundle_new_from_path:
 * @path: a bundle file
 * @error: a #GError
 *
 * Open a bundle by its path, like mf_bundle_new_from_fd().
 *
 * Returns: (transfer full): the bundle, or %NULL on failure
 */
MfBundle *
mf_bundle_new_from_path(const char *path, GError **error)
{
    MfBundle *bundle;

    g_return_val_if_fail(path != NULL, NULL);

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        g_set_error(error, MF_ERROR, MF_ERROR_FAILED, _("Unable to open %s: %s"), path, g_strerror(errno));
        return NULL;
    }
    bundle = mf_bundle_new_from_fd(fd, error);
    close(fd);
    if (bundle == NULL)
        g_prefix_error(error, "%s: ", path);
    return bundle;
}

/**
 * mf_bundle_get_images:
 * @bundle: an #MfBundle
 *
 * Returns: (transfer none) (element-type MfBundleImage): the images of the bundle
 */
GPtrArray *
mf_bundle_get_images(MfBundle *bundle)
{
    g_return_val_if_fail(MF_IS_BUNDLE(bundle), NULL);
    return GET_PRIVATE(bundle)->images;
}

/**
 * mf_bundle_find_image_for_identity:
 * @bundle: an #MfBundle
 * @identity: the identity to look for
 *
 * Find the image in the bundle that provides an identity.
 *
 * Returns: (transfer none) (nullable): the image, or %NULL if none provides it
 */
MfBundleImage *
mf_bundle_find_image_for_identity(MfBundle *bundle, const char *identity)
{
    GPtrArray *images;

    g_return_val_if_fail(MF_IS_BUNDLE(bundle), NULL);

    images = GET_PRIVATE(bundle)->images;
    for (guint i = 0; i < images->len; i++) {
        MfBundleImage *image = g_ptr_array_index(images, i);

        if (g_strcmp0(mf_manifest_get_identity(mf_bundle_image_get_manifest(image)), identity) == 0)
            return image;
    }

    return NULL;
}

/**
 * mf_bundle_is_signed:
 * @bundle: an #MfBundle
 *
 * Returns: whether the bundle carries a signature over its index. Makes NO claims about trust!
 */
gboolean
mf_bundle_is_signed(MfBundle *bundle)
{
    g_return_val_if_fail(MF_IS_BUNDLE(bundle), FALSE);
    return GET_PRIVATE(bundle)->is_signed;
}

/**
 * mf_zip_replace_signature:
 *
 * Write @signature into the bundle at @path, replacing any signature it has.
 */
static gboolean
mf_zip_replace_signature(const char *path, GBytes *signature, GError **error)
{
    gsize size;
    const void *data = g_bytes_get_data(signature, &size);
    zip_source_t *source;
    zip_int64_t index = -1;
    int zip_errno = 0;
    zip_t *zip;

    zip = zip_open(path, 0, &zip_errno);
    if (zip == NULL) {
        zip_error_t zerr;

        zip_error_init_with_code(&zerr, zip_errno);
        mf_set_error_from_zip(error, MF_ERROR_FAILED, &zerr, _("Unable to open the bundle for writing"));
        zip_error_fini(&zerr);
        return FALSE;
    }

    /* NOTE: libzip only reads the buffer in zip_close(), so the signature must stay alive until then. */
    source = zip_source_buffer(zip, data, size, 0);
    if (source != NULL) {
        index = zip_file_add(zip, SIGNATURE_NAME, source, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE);
        if (index < 0)
            zip_source_free(source);
    }
    if (index < 0 || zip_close(zip) != 0) {
        mf_set_error_from_zip(error, MF_ERROR_FAILED, zip_get_error(zip), _("Unable to write the signature"));
        zip_discard(zip);
        return FALSE;
    }

    return TRUE;
}

/**
 * mf_bundle_sign_file:
 * @path: a bundle file
 * @signer: an #MfSigner
 * @error: a #GError
 *
 * Sign a bundle in place, replacing any signature it has.
 *
 * We refuse to sign a bundle that we can not open, for example because a
 * file changed after its index was written.
 *
 * Returns: %TRUE if the bundle was signed
 */
gboolean
mf_bundle_sign_file(const char *path, MfSigner *signer, GError **error)
{
    g_autoptr(MfBundle) bundle = NULL;
    g_autoptr(GBytes) signature = NULL;

    g_return_val_if_fail(path != NULL, FALSE);
    g_return_val_if_fail(MF_IS_SIGNER(signer), FALSE);

    bundle = mf_bundle_new_from_path(path, error);
    if (bundle == NULL)
        return FALSE;

    signature = mf_signer_sign(signer, MF_SIGNATURE_NAMESPACE_BUNDLE, GET_PRIVATE(bundle)->index_data, error);
    if (signature == NULL)
        return FALSE;

    if (!mf_zip_replace_signature(path, signature, error)) {
        g_prefix_error(error, "%s: ", path);
        return FALSE;
    }

    return TRUE;
}

/**
 * mf_bundle_copy_image:
 * @bundle: the bundle
 * @image: the image to copy
 * @dest_fd: file descriptor to write the image to
 * @progress_func: (nullable) (scope call): called after every piece that was copied
 * @user_data: passed to @progress_func
 * @error: a #GError
 *
 * Copy an image out of the bundle, verifying it against its manifest on the
 * way. If the verification fails, what was written is invalid and should be discarded.
 *
 * Returns: %TRUE if the image was copied and verified
 */
gboolean
mf_bundle_copy_image(MfBundle *bundle,
                     MfBundleImage *image,
                     int dest_fd,
                     MfCopyProgressFunc progress_func,
                     gpointer user_data,
                     GError **error)
{
    g_autofree char *buffer = g_malloc(COPY_BUFFER_SIZE);
    g_autoptr(GChecksum) checksum = g_checksum_new(G_CHECKSUM_SHA256);
    g_autofree char *blake3 = NULL;
    blake3_hasher hasher;
    MfBundlePrivate *priv;
    const char *image_name;
    guint64 image_size;
    zip_file_t *file;
    guint64 total = 0;
    gboolean ret = FALSE;

    g_return_val_if_fail(MF_IS_BUNDLE(bundle), FALSE);
    g_return_val_if_fail(MF_IS_BUNDLE_IMAGE(image), FALSE);

    priv = GET_PRIVATE(bundle);
    image_name = mf_manifest_get_image_file(image->manifest);
    image_size = mf_manifest_get_image_size(image->manifest);
    blake3_hasher_init(&hasher);

    file = zip_fopen_index(priv->zip, image->image_index, 0);
    if (file == NULL) {
        mf_set_error_from_zip(error, MF_ERROR_INVALID_BUNDLE, zip_get_error(priv->zip), image_name);
        return FALSE;
    }

    for (;;) {
        zip_int64_t n = zip_fread(file, buffer, COPY_BUFFER_SIZE);

        if (n < 0) {
            mf_set_error_from_zip(error, MF_ERROR_INVALID_BUNDLE, zip_file_get_error(file), image_name);
            goto out;
        }
        if (n == 0)
            break;

        total += (guint64) n;
        if (total > image_size)
            break;

        g_checksum_update(checksum, (const guchar *) buffer, (gsize) n);
        blake3_hasher_update(&hasher, buffer, (gsize) n);
        if (dest_fd >= 0 && !mf_write_all(dest_fd, buffer, (gsize) n, error))
            goto out;
        if (progress_func != NULL)
            progress_func(total, user_data);
    }

    /* TODO: Also check that the dm-verity hash tree of the image produces its
     * root hash, before the image is ever mounted. */
    blake3 = mf_blake3_hex(&hasher);
    if (total != image_size ||
        g_strcmp0(g_checksum_get_string(checksum),
                  mf_manifest_get_image_checksum(image->manifest, MF_CHECKSUM_KIND_SHA256)) != 0 ||
        g_strcmp0(blake3, mf_manifest_get_image_checksum(image->manifest, MF_CHECKSUM_KIND_BLAKE3)) != 0) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_VERIFICATION_FAILED,
                    _("Image %s does not match what its manifest records"),
                    image_name);
        goto out;
    }

    ret = TRUE;

out:
    zip_fclose(file);
    return ret;
}

/**
 * mf_bundle_verify_image:
 * @bundle: the bundle
 * @image: the image to verify
 * @progress_func: (nullable) (scope call): called after every piece that was read
 * @user_data: passed to @progress_func
 * @error: a #GError
 *
 * Verify an image against its manifest without copying it anywhere.
 *
 * Returns: %TRUE if the image is what its manifest records
 */
gboolean
mf_bundle_verify_image(MfBundle *bundle,
                       MfBundleImage *image,
                       MfCopyProgressFunc progress_func,
                       gpointer user_data,
                       GError **error)
{
    return mf_bundle_copy_image(bundle, image, -1, progress_func, user_data, error);
}
