/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mf-bundle-writer.h"

#include <errno.h>
#include <string.h>
#include <glib/gi18n-lib.h>
#include <glib/gstdio.h>
#include <zip.h>
#include <appstream.h>
#include <systemd/sd-json.h>

#include "mf-bundle-private.h"
#include "mf-sdautoptr-private.h"
#include "mf-utils-private.h"

/**
 * MfBundleWriter:
 *
 * Writes bundles, and signs existing ones.
 *
 * Nothing reaches the disk before mf_bundle_writer_finish().
 */

constexpr char IMAGE_SUFFIX[] = ".raw";

/* a file that the index lists */
typedef struct {
    char *name;
    GBytes *data;
    char *sha256;
    char *blake3;
} MfIndexedFile;

static void
mf_indexed_file_free(MfIndexedFile *file)
{
    g_free(file->name);
    g_bytes_unref(file->data);
    g_free(file->sha256);
    g_free(file->blake3);
    g_free(file);
}

typedef struct {
    char *path;
    zip_t *zip;
    AsMetadata *catalog; /* holds the component of every image */
    GPtrArray *files;    /* of MfIndexedFile */
    GBytes *index;       /* set once nothing can be added anymore */
    GBytes *signature;
} MfBundleWriterPrivate;

G_DEFINE_TYPE_WITH_PRIVATE(MfBundleWriter, mf_bundle_writer, G_TYPE_OBJECT)

#define GET_PRIVATE(o) ((MfBundleWriterPrivate *) mf_bundle_writer_get_instance_private(o))

static void
mf_bundle_writer_finalize(GObject *object)
{
    auto priv = GET_PRIVATE(MF_BUNDLE_WRITER(object));

    /* the archive refers to the data below until it is gone */
    if (priv->zip != NULL)
        zip_discard(priv->zip);
    g_free(priv->path);
    g_clear_object(&priv->catalog);
    g_clear_pointer(&priv->files, g_ptr_array_unref);
    g_clear_pointer(&priv->index, g_bytes_unref);
    g_clear_pointer(&priv->signature, g_bytes_unref);

    G_OBJECT_CLASS(mf_bundle_writer_parent_class)->finalize(object);
}

static void
mf_bundle_writer_class_init(MfBundleWriterClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = mf_bundle_writer_finalize;
}

static void
mf_bundle_writer_init(MfBundleWriter *writer)
{
    MfBundleWriterPrivate *priv = GET_PRIVATE(writer);

    priv->files = g_ptr_array_new_with_free_func((GDestroyNotify) mf_indexed_file_free);
    priv->catalog = as_metadata_new();
    as_metadata_set_locale(priv->catalog, "ALL");
    as_metadata_set_format_style(priv->catalog, AS_FORMAT_STYLE_CATALOG);
    as_metadata_set_origin(priv->catalog, MF_BUNDLE_CATALOG_ORIGIN);
}

/**
 * mf_bundle_writer_open_archive:
 * @flags: how to open the archive, for zip_open()
 */
static MfBundleWriter *
mf_bundle_writer_open_archive(const char *path, int flags, GError **error)
{
    g_autoptr(MfBundleWriter) writer = g_object_new(MF_TYPE_BUNDLE_WRITER, NULL);
    MfBundleWriterPrivate *priv = GET_PRIVATE(writer);
    int zip_errno = 0;

    priv->path = g_strdup(path);
    priv->zip = zip_open(path, flags, &zip_errno);
    if (priv->zip == NULL) {
        zip_error_t zerr;

        zip_error_init_with_code(&zerr, zip_errno);
        mf_set_error_from_zip(error, MF_ERROR_FAILED, &zerr, _("Unable to open the bundle for writing"));
        zip_error_fini(&zerr);
        g_prefix_error(error, "%s: ", path);
        return NULL;
    }

    return g_steal_pointer(&writer);
}

/**
 * mf_bundle_writer_new:
 * @path: where to write the bundle
 * @error: a #GError
 *
 * Create a writer for a new bundle. A file at @path is only replaced once the
 * bundle is finished.
 *
 * Returns: (transfer full): a new #MfBundleWriter, or %NULL on error
 */
MfBundleWriter *
mf_bundle_writer_new(const char *path, GError **error)
{
    g_return_val_if_fail(path != NULL, NULL);
    return mf_bundle_writer_open_archive(path, ZIP_CREATE | ZIP_TRUNCATE, error);
}

/**
 * mf_bundle_writer_new_for_file:
 * @path: an existing bundle
 * @error: a #GError
 *
 * Create a writer to edit an existing bundle in place.
 *
 * We refuse a bundle that we can not open, for example because a file changed
 * after its index was written.
 *
 * Returns: (transfer full): a new #MfBundleWriter, or %NULL on error
 */
MfBundleWriter *
mf_bundle_writer_new_for_file(const char *path, GError **error)
{
    g_autoptr(MfBundle) bundle = NULL;
    MfBundleWriterPrivate *priv;
    MfBundleWriter *writer;

    g_return_val_if_fail(path != NULL, NULL);

    bundle = mf_bundle_new_from_path(path, error);
    if (bundle == NULL)
        return NULL;

    writer = mf_bundle_writer_open_archive(path, 0, error);
    if (writer == NULL)
        return NULL;

    priv = GET_PRIVATE(writer);
    priv->index = g_bytes_ref(mf_bundle_get_index_data(bundle));

    return writer;
}

/**
 * mf_bundle_writer_check_name:
 *
 * Check that we can add a file of this name to the archive.
 */
static gboolean
mf_bundle_writer_check_name(MfBundleWriter *writer, const char *name, GError **error)
{
    MfBundleWriterPrivate *priv = GET_PRIVATE(writer);

    if (!mf_bundle_file_name_is_valid(name)) {
        g_set_error(error, MF_ERROR, MF_ERROR_INVALID_BUNDLE, _("%s is not a valid name in a bundle"), name);
        return FALSE;
    }
    if (zip_name_locate(priv->zip, name, 0) >= 0) {
        g_set_error(error, MF_ERROR, MF_ERROR_INVALID_BUNDLE, _("The bundle contains %s twice"), name);
        return FALSE;
    }

    return TRUE;
}

/**
 * mf_bundle_writer_add_entry:
 * @source: the content of the file, which we take over
 * @method: how to compress the file, as a compression method of libzip
 *
 * Add a file to the archive.
 */
static gboolean
mf_bundle_writer_add_entry(MfBundleWriter *writer,
                           const char *name,
                           zip_source_t *source,
                           zip_int32_t method,
                           GError **error)
{
    MfBundleWriterPrivate *priv = GET_PRIVATE(writer);
    zip_int64_t index = zip_file_add(priv->zip, name, source, ZIP_FL_ENC_UTF_8);

    if (index < 0) {
        zip_source_free(source);
        mf_set_error_from_zip(error, MF_ERROR_FAILED, zip_get_error(priv->zip), name);
        return FALSE;
    }
    if (zip_set_file_compression(priv->zip, (zip_uint64_t) index, method, 0) != 0) {
        mf_set_error_from_zip(error, MF_ERROR_FAILED, zip_get_error(priv->zip), name);
        return FALSE;
    }

    return TRUE;
}

/**
 * mf_bundle_writer_add_bytes:
 *
 * Add a file from memory.
 *
 * NOTE: libzip only reads @data in zip_close(), so it must stay alive until then.
 */
static gboolean
mf_bundle_writer_add_bytes(MfBundleWriter *writer,
                           const char *name,
                           GBytes *data,
                           zip_int32_t method,
                           GError **error)
{
    MfBundleWriterPrivate *priv = GET_PRIVATE(writer);
    gsize size;
    gconstpointer buffer = g_bytes_get_data(data, &size);
    zip_source_t *source = zip_source_buffer(priv->zip, buffer, size, 0);

    if (source == NULL) {
        mf_set_error_from_zip(error, MF_ERROR_FAILED, zip_get_error(priv->zip), name);
        return FALSE;
    }

    return mf_bundle_writer_add_entry(writer, name, source, method, error);
}

/**
 * mf_bundle_writer_add_indexed_file:
 *
 * Add a file to the archive and to the index. We hash the same bytes that we
 * store, so the index can not disagree with the archive.
 */
static gboolean
mf_bundle_writer_add_indexed_file(MfBundleWriter *writer,
                                  const char *name,
                                  GBytes *data,
                                  zip_int32_t method,
                                  GError **error)
{
    MfBundleWriterPrivate *priv = GET_PRIVATE(writer);
    MfIndexedFile *file;

    if (!mf_bundle_writer_check_name(writer, name, error))
        return FALSE;
    if (!mf_bundle_writer_add_bytes(writer, name, data, method, error))
        return FALSE;

    file = g_new0(MfIndexedFile, 1);
    file->name = g_strdup(name);
    file->data = g_bytes_ref(data);
    file->sha256 = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, data);
    file->blake3 = mf_blake3_of_bytes(data);
    g_ptr_array_add(priv->files, file);

    return TRUE;
}

/**
 * mf_bundle_parse_component:
 * @manifest: the manifest of the image
 * @catalog: catalog XML with the component of the image
 *
 * Returns: (transfer full): the component of an image, which names the image
 *   as what installs it, or %NULL on error
 */
static AsComponent *
mf_bundle_parse_component(MfManifest *manifest, GBytes *catalog, GError **error)
{
    const char *component_id = mf_manifest_get_component_id(manifest);
    g_autofree char *ref = mf_bundle_build_image_ref(manifest);
    g_autoptr(AsMetadata) metadata = as_metadata_new();
    g_autoptr(AsBundle) bundle = as_bundle_new();
    g_autoptr(GError) local_error = NULL;
    AsComponentBox *components;
    AsComponent *component;

    as_metadata_set_locale(metadata, "ALL");
    as_metadata_set_format_style(metadata, AS_FORMAT_STYLE_CATALOG);
    if (!as_metadata_parse_bytes(metadata, catalog, AS_FORMAT_KIND_XML, &local_error)) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_INVALID_BUNDLE,
                    /* TRANSLATORS: the component, and why its catalog data can not be read */
                    _("The catalog data of %s is not valid: %s"),
                    component_id,
                    local_error->message);
        return NULL;
    }

    components = as_metadata_get_components(metadata);
    if (as_component_box_len(components) != 1) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_INVALID_BUNDLE,
                    _("The catalog data of %s has to hold exactly one component"),
                    component_id);
        return NULL;
    }
    component = as_component_box_index(components, 0);
    if (g_strcmp0(as_component_get_id(component), component_id) != 0) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_INVALID_BUNDLE,
                    /* TRANSLATORS: the component of an image, and the component its catalog data describes */
                    _("The catalog data of %s describes %s"),
                    component_id,
                    as_component_get_id(component));
        return NULL;
    }

    /* MetaInfo may ship bundles too, but only we can name the image */
    g_ptr_array_set_size(as_component_get_bundles(component), 0);
    as_bundle_set_kind(bundle, AS_BUNDLE_KIND_MANIFOLD);
    as_bundle_set_id(bundle, ref);
    as_component_add_bundle(component, bundle);

    return g_object_ref(component);
}

/**
 * mf_bundle_build_manifest_name:
 *
 * Returns: the name of the manifest of an image in a bundle
 */
static char *
mf_bundle_build_manifest_name(const char *image_name)
{
    g_autofree char *stem = NULL;

    if (!g_str_has_suffix(image_name, IMAGE_SUFFIX))
        return g_strconcat(image_name, MF_BUNDLE_MANIFEST_SUFFIX, NULL);

    stem = g_strndup(image_name, strlen(image_name) - strlen(IMAGE_SUFFIX));
    return g_strconcat(stem, MF_BUNDLE_MANIFEST_SUFFIX, NULL);
}

/**
 * mf_bundle_writer_add_image:
 * @writer: an #MfBundleWriter
 * @image_path: the image file
 * @manifest: the manifest of the image
 * @catalog: AppStream catalog XML that holds the component of the image
 * @error: a #GError
 *
 * Add an image with its manifest and its component. An image is stored under
 * the name its manifest records, and has to be as large as that says. Its
 * hashes are only verified when the bundle is read.
 *
 * Whatever the catalog XML says about the origin or the bundles of the
 * component is replaced.
 *
 * The image file is only read by mf_bundle_writer_finish(), so it has to stay
 * in place until then. If the image is refused, the writer stays as it was.
 *
 * Returns: %TRUE if the image was added
 */
gboolean
mf_bundle_writer_add_image(MfBundleWriter *writer,
                           const char *image_path,
                           GBytes *manifest,
                           GBytes *catalog,
                           GError **error)
{
    MfBundleWriterPrivate *priv;
    g_autoptr(MfManifest) parsed = NULL;
    g_autoptr(AsComponent) component = NULL;
    g_autofree char *manifest_name = NULL;
    const char *image_name;
    zip_source_t *source;
    GStatBuf st;

    g_return_val_if_fail(MF_IS_BUNDLE_WRITER(writer), FALSE);
    g_return_val_if_fail(image_path != NULL, FALSE);
    g_return_val_if_fail(manifest != NULL, FALSE);
    g_return_val_if_fail(catalog != NULL, FALSE);
    priv = GET_PRIVATE(writer);
    g_return_val_if_fail(priv->zip != NULL && priv->index == NULL, FALSE);

    parsed = mf_manifest_new_from_bytes(manifest, error);
    if (parsed == NULL)
        return FALSE;

    image_name = mf_manifest_get_image_file(parsed);
    manifest_name = mf_bundle_build_manifest_name(image_name);
    if (!mf_bundle_writer_check_name(writer, image_name, error) ||
        !mf_bundle_writer_check_name(writer, manifest_name, error))
        return FALSE;

    if (g_stat(image_path, &st) != 0) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_FAILED,
                    _("Unable to read %s: %s"),
                    image_path,
                    g_strerror(errno));
        return FALSE;
    }
    if ((guint64) st.st_size != mf_manifest_get_image_size(parsed)) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_VERIFICATION_FAILED,
                    _("Image %s does not have the size its manifest records"),
                    image_name);
        return FALSE;
    }

    component = mf_bundle_parse_component(parsed, catalog, error);
    if (component == NULL)
        return FALSE;

    /* images are compressed already */
    source = zip_source_file(priv->zip, image_path, 0, -1);
    if (source == NULL) {
        mf_set_error_from_zip(error, MF_ERROR_FAILED, zip_get_error(priv->zip), image_path);
        return FALSE;
    }
    if (!mf_bundle_writer_add_entry(writer, image_name, source, ZIP_CM_STORE, error))
        return FALSE;
    if (!mf_bundle_writer_add_indexed_file(writer, manifest_name, manifest, ZIP_CM_DEFLATE, error))
        return FALSE;

    as_metadata_add_component(priv->catalog, component);
    return TRUE;
}

/**
 * mf_bundle_writer_add_tree:
 * @dir: a directory to add the files of
 * @prefix: the path to store them below in the bundle
 *
 * Add the files below a directory, in a stable order. We only map them, as
 * screenshots and videos can be large. Icons and media are compressed already.
 */
static gboolean
mf_bundle_writer_add_tree(MfBundleWriter *writer, const char *dir, const char *prefix, GError **error)
{
    g_autoptr(GDir) gdir = NULL;
    g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
    const char *name;

    gdir = g_dir_open(dir, 0, error);
    if (gdir == NULL)
        return FALSE;
    while ((name = g_dir_read_name(gdir)) != NULL)
        g_ptr_array_add(names, g_strdup(name));
    g_ptr_array_sort_values(names, (GCompareFunc) g_strcmp0);

    for (guint i = 0; i < names->len; i++) {
        const char *entry = g_ptr_array_index(names, i);
        g_autofree char *path = g_build_filename(dir, entry, NULL);
        g_autofree char *bundle_name = g_strconcat(prefix, entry, NULL);
        g_autoptr(GMappedFile) mapped = NULL;
        g_autoptr(GBytes) data = NULL;

        if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
            g_autofree char *subdir_prefix = g_strconcat(bundle_name, "/", NULL);

            if (!mf_bundle_writer_add_tree(writer, path, subdir_prefix, error))
                return FALSE;
            continue;
        }

        mapped = g_mapped_file_new(path, FALSE, error);
        if (mapped == NULL)
            return FALSE;
        data = g_mapped_file_get_bytes(mapped);
        if (!mf_bundle_writer_add_indexed_file(writer, bundle_name, data, ZIP_CM_STORE, error))
            return FALSE;
    }

    return TRUE;
}

/**
 * mf_bundle_writer_add_catalog_assets:
 * @writer: an #MfBundleWriter
 * @icons_dir: (nullable): a directory with the cached icons of the components
 * @media_dir: (nullable): a directory with their screenshots and videos
 * @error: a #GError
 *
 * Add the files that the components of the images refer to, laid out the way
 * appstream-compose writes them.
 *
 * The files have to stay unchanged until mf_bundle_writer_finish(). If this
 * fails, none of them are added.
 *
 * Returns: %TRUE if the files were added
 */
gboolean
mf_bundle_writer_add_catalog_assets(MfBundleWriter *writer,
                                    const char *icons_dir,
                                    const char *media_dir,
                                    GError **error)
{
    MfBundleWriterPrivate *priv;
    guint n_files;

    g_return_val_if_fail(MF_IS_BUNDLE_WRITER(writer), FALSE);
    priv = GET_PRIVATE(writer);
    g_return_val_if_fail(priv->zip != NULL && priv->index == NULL, FALSE);

    n_files = priv->files->len;
    if ((icons_dir == NULL ||
         mf_bundle_writer_add_tree(writer, icons_dir, MF_BUNDLE_CATALOG_ICONS_PREFIX, error)) &&
        (media_dir == NULL ||
         mf_bundle_writer_add_tree(writer, media_dir, MF_BUNDLE_CATALOG_MEDIA_PREFIX, error)))
        return TRUE;

    /* libzip writes nothing before zip_close(), so we can take back what we added */
    for (guint i = n_files; i < priv->files->len; i++) {
        MfIndexedFile *file = g_ptr_array_index(priv->files, i);

        zip_delete(priv->zip, (zip_uint64_t) zip_name_locate(priv->zip, file->name, 0));
    }
    g_ptr_array_set_size(priv->files, n_files);

    return FALSE;
}

/**
 * mf_indexed_file_compare:
 */
static int
mf_indexed_file_compare(gconstpointer a, gconstpointer b)
{
    const MfIndexedFile *file_a = *((const MfIndexedFile **) a);
    const MfIndexedFile *file_b = *((const MfIndexedFile **) b);

    return g_strcmp0(file_a->name, file_b->name);
}

/**
 * mf_bundle_writer_write_catalog:
 *
 * Add the catalog with the components of all images.
 */
static gboolean
mf_bundle_writer_write_catalog(MfBundleWriter *writer, GError **error)
{
    MfBundleWriterPrivate *priv = GET_PRIVATE(writer);
    g_autoptr(GError) local_error = NULL;
    g_autoptr(GBytes) data = NULL;
    char *xml;

    if (as_component_box_len(as_metadata_get_components(priv->catalog)) == 0) {
        g_set_error_literal(error, MF_ERROR, MF_ERROR_INVALID_BUNDLE, _("The bundle contains no images"));
        return FALSE;
    }

    xml = as_metadata_components_to_catalog(priv->catalog, AS_FORMAT_KIND_XML, &local_error);
    if (xml == NULL) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_FAILED,
                    _("Unable to write the catalog of the bundle: %s"),
                    local_error->message);
        return FALSE;
    }
    data = g_bytes_new_take(xml, strlen(xml));

    return mf_bundle_writer_add_indexed_file(writer, MF_BUNDLE_CATALOG_NAME, data, ZIP_CM_DEFLATE, error);
}

/**
 * mf_bundle_writer_write_index:
 *
 * Add the index, which lists all files that were added so far.
 */
static gboolean
mf_bundle_writer_write_index(MfBundleWriter *writer, GError **error)
{
    MfBundleWriterPrivate *priv = GET_PRIVATE(writer);
    g_autoptr(sd_json_variant) files = NULL;
    g_autoptr(sd_json_variant) index = NULL;
    g_autoptr(GBytes) data = NULL;
    char *formatted = NULL;
    int r;

    g_ptr_array_sort(priv->files, mf_indexed_file_compare);
    for (guint i = 0; i < priv->files->len; i++) {
        MfIndexedFile *file = g_ptr_array_index(priv->files, i);

        r = sd_json_variant_append_arraybo(&files,
                                           SD_JSON_BUILD_PAIR_STRING("name", file->name),
                                           SD_JSON_BUILD_PAIR_UNSIGNED("size", g_bytes_get_size(file->data)),
                                           SD_JSON_BUILD_PAIR_STRING("blake3", file->blake3),
                                           SD_JSON_BUILD_PAIR_STRING("sha256", file->sha256));
        if (r < 0)
            goto fail;
    }

    r = sd_json_buildo(&index,
                       SD_JSON_BUILD_PAIR_INTEGER("format", MF_BUNDLE_FORMAT),
                       SD_JSON_BUILD_PAIR_VARIANT("files", files));
    if (r < 0)
        goto fail;
    r = sd_json_variant_format(index, SD_JSON_FORMAT_PRETTY | SD_JSON_FORMAT_NEWLINE, &formatted);
    if (r < 0)
        goto fail;
    data = g_bytes_new_take(formatted, strlen(formatted));

    if (!mf_bundle_writer_add_bytes(writer, MF_BUNDLE_INDEX_NAME, data, ZIP_CM_DEFLATE, error))
        return FALSE;
    priv->index = g_steal_pointer(&data);

    return TRUE;

fail:
    g_set_error(error,
                MF_ERROR,
                MF_ERROR_FAILED,
                _("Unable to write %s: %s"),
                MF_BUNDLE_INDEX_NAME,
                g_strerror(-r));
    return FALSE;
}

/**
 * mf_bundle_writer_replace_signature:
 * @signature: (nullable): the new signature
 *
 * Replace the signature of the bundle. Without a new one, the old one is removed.
 */
static gboolean
mf_bundle_writer_replace_signature(MfBundleWriter *writer, GBytes *signature, GError **error)
{
    MfBundleWriterPrivate *priv = GET_PRIVATE(writer);
    zip_int64_t index = zip_name_locate(priv->zip, MF_BUNDLE_SIGNATURE_NAME, 0);

    if (index >= 0 && zip_delete(priv->zip, (zip_uint64_t) index) != 0) {
        mf_set_error_from_zip(error,
                              MF_ERROR_FAILED,
                              zip_get_error(priv->zip),
                              _("Unable to remove the old signature"));
        return FALSE;
    }

    g_clear_pointer(&priv->signature, g_bytes_unref);
    if (signature == NULL)
        return TRUE;
    priv->signature = g_bytes_ref(signature);

    return mf_bundle_writer_add_bytes(writer,
                                      MF_BUNDLE_SIGNATURE_NAME,
                                      priv->signature,
                                      ZIP_CM_DEFLATE,
                                      error);
}

/**
 * mf_bundle_writer_finish:
 * @writer: an #MfBundleWriter
 * @signer: (nullable): the key to sign the bundle with
 * @error: a #GError
 *
 * Write the bundle to disk. Without a @signer, the bundle ends up unsigned:
 * we never keep a signature that we did not just make, as the signature is
 * the last thing that has to be redone if a bundle changes.
 *
 * If this fails, nothing was written and the writer stays open, so we can be
 * called again. That way, a caller can still write an unsigned bundle if
 * signing failed.
 *
 * Returns: %TRUE if the bundle was written
 */
gboolean
mf_bundle_writer_finish(MfBundleWriter *writer, MfSigner *signer, GError **error)
{
    MfBundleWriterPrivate *priv;
    g_autoptr(GBytes) signature = NULL;

    g_return_val_if_fail(MF_IS_BUNDLE_WRITER(writer), FALSE);
    g_return_val_if_fail(signer == NULL || MF_IS_SIGNER(signer), FALSE);
    priv = GET_PRIVATE(writer);
    g_return_val_if_fail(priv->zip != NULL, FALSE);

    if (priv->index == NULL) {
        if (!mf_bundle_writer_write_catalog(writer, error))
            return FALSE;
        if (!mf_bundle_writer_write_index(writer, error))
            return FALSE;
    }

    if (signer != NULL) {
        signature = mf_signer_sign(signer, MF_SIGNATURE_SCOPE_BUNDLE, priv->index, error);
        if (signature == NULL)
            return FALSE;
    }
    if (!mf_bundle_writer_replace_signature(writer, signature, error))
        return FALSE;

    if (zip_close(priv->zip) != 0) {
        mf_set_error_from_zip(error,
                              MF_ERROR_FAILED,
                              zip_get_error(priv->zip),
                              _("Unable to write the bundle"));
        g_prefix_error(error, "%s: ", priv->path);
        return FALSE;
    }
    priv->zip = NULL;

    return TRUE;
}
