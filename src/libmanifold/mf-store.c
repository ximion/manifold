/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mf-store.h"
#include "mf-store-private.h"

#include <string.h>

#include "mf-component-private.h"
#include "mf-error.h"
#include "mf-utils.h"
#include "mf-utils-private.h"

/**
 * MfStore:
 *
 * Read-only view on the installed software components store, as written
 * by the management daemon. What is installed is read once, by
 * mf_store_load() or by the first method that needs it, and only read
 * again on the next mf_store_load().
 * Symbolic links are never followed blindly: each target is read and
 * validated first, and only then opened by that name.
 *
 * The Manifold store layout:
 *
 *   images/by-verity/<hash>.raw           every image, named by its verity root hash
 *   apps/<id>/<version>-<revision>/       an installed, launchable application
 *   plugins/<id>/<version>-<revision>/    a plugin of an application
 *   runtimes/<id>/<version>-<revision>/   a core, platform, kit or corresponding SDK
 *   <dir>/<id>/current                    link to the active version in use
 *
 * A version directory holds a link to its own image (`app.raw`, or
 * `runtime.raw` for a runtime), `base`, a link to the version directory of
 * the layer below, `manifest.json`, and the markers `origin` and `mode`.
 */

typedef struct {
    char *path;
    GPtrArray *components; /* (element-type MfComponent): what was loaded, or %NULL */
} MfStorePrivate;

enum {
    PROP_0,
    PROP_PATH,
    N_PROPS
};

static GParamSpec *properties[N_PROPS];

G_DEFINE_TYPE_WITH_PRIVATE(MfStore, mf_store, G_TYPE_OBJECT)

#define GET_PRIVATE(o) ((MfStorePrivate *) mf_store_get_instance_private(o))

/* the prefixes a link of a version directory must have */
#define COMPONENT_TO_IMAGE_PREFIX MF_STORE_COMPONENT_TO_IMAGES "/"
#define COMPONENT_TO_ROOT_PREFIX  MF_STORE_COMPONENT_TO_ROOT "/"

/**
 * mf_is_version_dir_name:
 *
 * Returns: whether @name has the form of a version directory, `<version>-<revision>`
 */
static gboolean
mf_is_version_dir_name(const char *name)
{
    g_autofree char *version = NULL;
    guint64 revision;

    return mf_split_full_version(name, &version, &revision);
}

static const char *const SUBDIRS[] = {
    MF_STORE_RUNTIMES_SUBDIR,
    MF_STORE_APPS_SUBDIR,
    MF_STORE_PLUGINS_SUBDIR,
};

/**
 * mf_store_hash_from_image_name:
 * @name: a file name of the form <hash>.raw
 *
 * Returns: (transfer full) (nullable): The hash @name carries, or %NULL
 */
char *
mf_store_hash_from_image_name(const char *name)
{
    g_autofree char *hash = NULL;

    if (!g_str_has_suffix(name, MF_STORE_IMAGE_SUFFIX))
        return NULL;
    hash = g_strndup(name, strlen(name) - strlen(MF_STORE_IMAGE_SUFFIX));
    if (!mf_hex_hash_is_valid(hash))
        return NULL;

    return g_steal_pointer(&hash);
}

/**
 * mf_store_finalize:
 */
static void
mf_store_finalize(GObject *object)
{
    MfStorePrivate *priv = GET_PRIVATE(MF_STORE(object));

    g_free(priv->path);
    g_clear_pointer(&priv->components, g_ptr_array_unref);

    G_OBJECT_CLASS(mf_store_parent_class)->finalize(object);
}

/**
 * mf_store_get_property:
 */
static void
mf_store_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
    MfStorePrivate *priv = GET_PRIVATE(MF_STORE(object));

    switch (prop_id) {
    case PROP_PATH:
        g_value_set_string(value, priv->path);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

/**
 * mf_store_set_property:
 */
static void
mf_store_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
    MfStorePrivate *priv = GET_PRIVATE(MF_STORE(object));

    switch (prop_id) {
    case PROP_PATH:
        g_free(priv->path);
        priv->path = g_value_dup_string(value);
        if (priv->path == NULL)
            priv->path = g_strdup(MF_STORAGE_DIR);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

/**
 * mf_store_class_init:
 */
static void
mf_store_class_init(MfStoreClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = mf_store_finalize;
    object_class->get_property = mf_store_get_property;
    object_class->set_property = mf_store_set_property;

    /**
     * MfStore:path:
     *
     * The root directory of the store.
     */
    properties[PROP_PATH] = g_param_spec_string("path",
                                                NULL,
                                                NULL,
                                                MF_STORAGE_DIR,
                                                G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                                                    G_PARAM_STATIC_STRINGS);
    g_object_class_install_properties(object_class, N_PROPS, properties);
}

/**
 * mf_store_init:
 */
static void
mf_store_init(MfStore *store)
{
}

/**
 * mf_store_new:
 *
 * Returns: (transfer full): the store of the host, below `/var/lib/manifold`
 */
MfStore *
mf_store_new(void)
{
    return g_object_new(MF_TYPE_STORE, NULL);
}

/**
 * mf_store_new_for_path:
 * @path: the root directory of a store
 *
 * Returns: (transfer full): a store that is somewhere else
 */
MfStore *
mf_store_new_for_path(const char *path)
{
    g_return_val_if_fail(path != NULL, NULL);
    return g_object_new(MF_TYPE_STORE, "path", path, NULL);
}

/**
 * mf_store_get_path:
 * @store: an #MfStore
 *
 * Returns: the root directory of the store
 */
const char *
mf_store_get_path(MfStore *store)
{
    g_return_val_if_fail(MF_IS_STORE(store), NULL);
    return GET_PRIVATE(store)->path;
}

/**
 * mf_store_subdir_for_kind:
 * @kind: a component kind
 * @sdk: whether the component is an SDK
 *
 * Returns: the store subdir that components of this kind are installed into.
 */
const char *
mf_store_subdir_for_kind(MfComponentKind kind, gboolean sdk)
{
    if (sdk)
        return MF_STORE_RUNTIMES_SUBDIR;
    switch (kind) {
    case MF_COMPONENT_KIND_APP:
        return MF_STORE_APPS_SUBDIR;
    case MF_COMPONENT_KIND_PLUGIN:
        return MF_STORE_PLUGINS_SUBDIR;
    default:
        return MF_STORE_RUNTIMES_SUBDIR;
    }
}

/**
 * mf_store_image_link_for_subdir:
 * @subdir: the subdirectory of the store a component is installed below
 *
 * Returns: the name of the link to its own image in the version directory of the component
 */
const char *
mf_store_image_link_for_subdir(const char *subdir)
{
    return g_str_equal(subdir, MF_STORE_RUNTIMES_SUBDIR) ? MF_STORE_RUNTIME_IMAGE_LINK
                                                         : MF_STORE_APP_IMAGE_LINK;
}

/**
 * mf_read_marker_file:
 *
 * Returns: the text content of a small file of a version directory, without surrounding space, or %NULL
 */
static char *
mf_read_marker_file(const char *version_dir, const char *name)
{
    g_autofree char *path = g_build_filename(version_dir, name, NULL);
    g_autofree char *content = NULL;

    if (!g_file_get_contents(path, &content, NULL, NULL))
        return NULL;
    return g_strstrip(g_steal_pointer(&content));
}

/**
 * mf_read_image_link:
 *
 * Read the link of a version directory to its own image, which has to point
 * to an image in the image store.
 *
 * Returns: (transfer full): the root hash of the image, or %NULL on error
 */
static char *
mf_read_image_link(const char *version_dir, const char *link_name, GError **error)
{
    g_autofree char *path = g_build_filename(version_dir, link_name, NULL);
    g_autofree char *target = g_file_read_link(path, error);

    if (target == NULL)
        return NULL;
    if (!g_str_has_prefix(target, COMPONENT_TO_IMAGE_PREFIX)) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_FAILED,
                    "Link %s does not point into the image store: %s",
                    path,
                    target);
        return NULL;
    }

    char *hash = mf_store_hash_from_image_name(target + strlen(COMPONENT_TO_IMAGE_PREFIX));
    if (hash == NULL) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_FAILED,
                    "Link %s does not name an image by its root hash: %s",
                    path,
                    target);
        return NULL;
    }

    return hash;
}

/**
 * mf_read_manifest_at:
 *
 * Returns: (transfer full): the manifest of a version directory, if it is
 *   the manifest of that component and version, or %NULL on error
 */
static MfManifest *
mf_read_manifest_at(const char *version_dir, const char *component_id, const char *version, GError **error)
{
    g_autofree char *manifest_path = g_build_filename(version_dir, MF_STORE_MANIFEST_NAME, NULL);
    g_autoptr(MfManifest) manifest = NULL;

    manifest = mf_manifest_new_from_file(manifest_path, error);
    if (manifest == NULL)
        return NULL;
    if (g_strcmp0(mf_manifest_get_component_id(manifest), component_id) != 0 ||
        g_strcmp0(mf_manifest_get_version(manifest), version) != 0) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_FAILED,
                    "%s says it is something else (expected: %s/%s)",
                    manifest_path,
                    component_id,
                    version);
        return NULL;
    }

    return g_steal_pointer(&manifest);
}

/**
 * mf_read_version_dir_at:
 *
 * Read the component of one version directory, from its links, markers and manifest.
 */
static MfComponent *
mf_read_version_dir_at(const char *root,
                       const char *subdir,
                       const char *component_id,
                       const char *version_name,
                       GError **error)
{
    g_autofree char *component_dir = g_build_filename(root, subdir, component_id, NULL);
    g_autofree char *version_dir = g_build_filename(component_dir, version_name, NULL);
    g_autofree char *current_path = g_build_filename(component_dir, MF_STORE_CURRENT_NAME, NULL);
    g_autofree char *current_target = NULL;
    g_autofree char *version = NULL;
    g_autofree char *verity_hash = NULL;
    g_autofree char *mode = NULL;
    g_autofree char *origin = NULL;
    g_autoptr(MfManifest) manifest = NULL;
    MfComponent *component;
    guint64 revision;

    if (!mf_split_full_version(version_name, &version, &revision)) {
        g_set_error(error, MF_ERROR, MF_ERROR_FAILED, "%s is not a version directory", version_dir);
        return NULL;
    }
    verity_hash = mf_read_image_link(version_dir, mf_store_image_link_for_subdir(subdir), error);
    if (verity_hash == NULL)
        return NULL;
    manifest = mf_read_manifest_at(version_dir, component_id, version, error);
    if (manifest == NULL)
        return NULL;

    current_target = g_file_read_link(current_path, NULL);
    mode = mf_read_marker_file(version_dir, MF_STORE_MODE_NAME);
    origin = mf_read_marker_file(version_dir, MF_STORE_ORIGIN_NAME);

    component = mf_component_new(component_id,
                                 mf_manifest_get_kind(manifest),
                                 version,
                                 revision,
                                 mf_manifest_is_sdk(manifest),
                                 mf_manifest_get_identity(manifest),
                                 verity_hash,
                                 g_strcmp0(current_target, version_name) == 0,
                                 mode != NULL ? mf_install_mode_from_string(mode) : MF_INSTALL_MODE_MANUAL,
                                 origin != NULL ? origin : MF_STORE_LOCAL_ORIGIN);
    mf_component_set_manifest(component, manifest);
    return component;
}

/**
 * mf_find_component_subdir_at:
 *
 * Returns: the subdirectory a component is installed below, or %NULL on error.
 */
static const char *
mf_find_component_subdir_at(const char *root, const char *component_id, GError **error)
{
    for (size_t i = 0; i < G_N_ELEMENTS(SUBDIRS); i++) {
        g_autofree char *component_dir = g_build_filename(root, SUBDIRS[i], component_id, NULL);

        if (g_file_test(component_dir, G_FILE_TEST_IS_DIR))
            return SUBDIRS[i];
    }

    g_set_error(error, MF_ERROR, MF_ERROR_NOT_FOUND, "%s is not installed", component_id);
    return NULL;
}

/**
 * mf_version_dir_name_at:
 *
 * Find the directory of a version of a component: the one `current` points
 * to, or the newest revision of an exact version.
 */
static char *
mf_version_dir_name_at(const char *root,
                       const char *subdir,
                       const char *component_id,
                       const char *version,
                       GError **error)
{
    g_autofree char *component_dir = g_build_filename(root, subdir, component_id, NULL);
    g_autofree char *newest = NULL;
    g_autoptr(GDir) dir = NULL;
    guint64 newest_revision = 0;
    const char *name;

    if (version == NULL) {
        g_autofree char *current_path = g_build_filename(component_dir, MF_STORE_CURRENT_NAME, NULL);
        char *target = g_file_read_link(current_path, NULL);

        if (!mf_is_version_dir_name(target)) {
            g_free(target);
            g_set_error(error,
                        MF_ERROR,
                        MF_ERROR_NOT_FOUND,
                        "%s is installed, but has no current version",
                        component_id);
            return NULL;
        }
        return target;
    }

    dir = g_dir_open(component_dir, 0, error);
    if (dir == NULL)
        return NULL;
    while ((name = g_dir_read_name(dir)) != NULL) {
        g_autofree char *path = g_build_filename(component_dir, name, NULL);
        g_autofree char *dir_version = NULL;
        guint64 revision;

        if (g_file_test(path, G_FILE_TEST_IS_SYMLINK) || !g_file_test(path, G_FILE_TEST_IS_DIR))
            continue;
        if (!mf_split_full_version(name, &dir_version, &revision) || !g_str_equal(dir_version, version))
            continue;
        if (newest == NULL || revision > newest_revision) {
            g_free(newest);
            newest = g_strdup(name);
            newest_revision = revision;
        }
    }

    if (newest == NULL)
        g_set_error(error, MF_ERROR, MF_ERROR_NOT_FOUND, "%s %s is not installed", component_id, version);
    return g_steal_pointer(&newest);
}

/**
 * mf_component_make_version_dir_name:
 */
static char *
mf_component_make_version_dir_name(MfComponent *component)
{
    return mf_build_full_version(mf_component_get_version(component), mf_component_get_revision(component));
}

/**
 * mf_component_dir_at:
 */
static char *
mf_component_dir_at(const char *root, MfComponent *component)
{
    g_autofree char *version_name = mf_component_make_version_dir_name(component);

    return g_build_filename(
        root,
        mf_store_subdir_for_kind(mf_component_get_kind(component), mf_component_is_sdk(component)),
        mf_component_get_id(component),
        version_name,
        NULL);
}

/**
 * mf_store_read_component_at:
 * @root: the root directory of a store
 * @component_id: identifier of a component
 * @version: (nullable): the version wanted, or %NULL for the current one
 * @error: a #GError
 *
 * Read one installed version of a component directly, without loading the
 * store: the one `current` names, or an exact version, in its newest
 * revision.
 *
 * Returns: (transfer full): the component, or %NULL on error
 */
MfComponent *
mf_store_read_component_at(const char *root, const char *component_id, const char *version, GError **error)
{
    g_autofree char *version_name = NULL;
    const char *subdir;

    g_return_val_if_fail(mf_component_id_is_valid(component_id), NULL);
    g_return_val_if_fail(version == NULL || mf_version_is_valid(version), NULL);

    subdir = mf_find_component_subdir_at(root, component_id, error);
    if (subdir == NULL)
        return NULL;
    version_name = mf_version_dir_name_at(root, subdir, component_id, version, error);
    if (version_name == NULL)
        return NULL;
    return mf_read_version_dir_at(root, subdir, component_id, version_name, error);
}

/**
 * mf_store_read_base_link_at:
 * @root: the root directory of a store
 * @component: an installed component
 * @error: a #GError
 *
 * Read the `base` link of a component, without reading what it names.
 *
 * Returns: (transfer full): the subdirectory of the store, the component and
 *   the version directory or `current` the link names, or %NULL if the
 *   component has no base or the link is not valid
 */
char **
mf_store_read_base_link_at(const char *root, MfComponent *component, GError **error)
{
    g_autofree char *version_dir = mf_component_dir_at(root, component);
    g_autofree char *link_path = g_build_filename(version_dir, MF_STORE_BASE_LINK, NULL);
    g_autofree char *target = NULL;
    g_auto(GStrv) parts = NULL;
    g_autoptr(GError) local_error = NULL;

    target = g_file_read_link(link_path, &local_error);
    if (g_error_matches(local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_NOT_FOUND,
                    "%s %s has no base",
                    mf_component_get_id(component),
                    mf_component_get_version(component));
        return NULL;
    }
    if (target == NULL) {
        g_propagate_error(error, g_steal_pointer(&local_error));
        return NULL;
    }

    /* ../../../<dir>/<component>/<current or version> */
    if (!g_str_has_prefix(target, COMPONENT_TO_ROOT_PREFIX)) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_FAILED,
                    "Link %s does not point into the store: %s",
                    link_path,
                    target);
        return NULL;
    }
    parts = g_strsplit(target + strlen(COMPONENT_TO_ROOT_PREFIX), "/", -1);
    if (g_strv_length(parts) != 3 || !mf_component_id_is_valid(parts[1]) ||
        (!g_str_equal(parts[2], MF_STORE_CURRENT_NAME) && !mf_is_version_dir_name(parts[2])) ||
        (!g_str_equal(parts[0], MF_STORE_RUNTIMES_SUBDIR) && !g_str_equal(parts[0], MF_STORE_APPS_SUBDIR))) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_FAILED,
                    "Link %s does not name a version of a component: %s",
                    link_path,
                    target);
        return NULL;
    }

    return g_steal_pointer(&parts);
}

/**
 * mf_store_read_base_at:
 * @root: the root directory of a store
 * @component: an installed component
 * @error: a #GError
 *
 * Like mf_store_get_base(), but reads the base directly, without loading
 * the store.
 *
 * Returns: (transfer full): the base, or %NULL if the component has none
 *   or it could not be read
 */
MfComponent *
mf_store_read_base_at(const char *root, MfComponent *component, GError **error)
{
    g_auto(GStrv) parts = mf_store_read_base_link_at(root, component, error);

    if (parts == NULL)
        return NULL;
    if (g_str_equal(parts[2], MF_STORE_CURRENT_NAME))
        return mf_store_read_component_at(root, parts[1], NULL, error);
    return mf_read_version_dir_at(root, parts[0], parts[1], parts[2], error);
}

/**
 * mf_store_image_path_at:
 * @root: the root directory of a store
 * @verity_hash: the root hash of an image
 *
 * Returns: (transfer full): where the image with that root hash is, or would be
 */
char *
mf_store_image_path_at(const char *root, const char *verity_hash)
{
    g_autofree char *file_name = g_strconcat(verity_hash, MF_STORE_IMAGE_SUFFIX, NULL);

    return g_build_filename(root, MF_STORE_IMAGES_SUBDIR, file_name, NULL);
}

/**
 * mf_scan_at:
 *
 * Read every version directory of the store. Version directories that can
 * not be read are skipped, as if they were not installed.
 *
 * Returns: (element-type MfComponent): what is installed
 */
static GPtrArray *
mf_scan_at(const char *root, GError **error)
{
    g_autoptr(GPtrArray) components = g_ptr_array_new_with_free_func(g_object_unref);

    for (size_t i = 0; i < G_N_ELEMENTS(SUBDIRS); i++) {
        g_autofree char *base_dir = g_build_filename(root, SUBDIRS[i], NULL);
        g_autoptr(GDir) dir = NULL;
        const char *id;

        if (!g_file_test(base_dir, G_FILE_TEST_IS_DIR))
            continue;
        dir = g_dir_open(base_dir, 0, error);
        if (dir == NULL)
            return NULL;

        while ((id = g_dir_read_name(dir)) != NULL) {
            g_autofree char *component_dir = g_build_filename(base_dir, id, NULL);
            g_autoptr(GDir) versions = NULL;
            const char *version_name;

            if (!mf_component_id_is_valid(id) || !g_file_test(component_dir, G_FILE_TEST_IS_DIR))
                continue;
            versions = g_dir_open(component_dir, 0, error);
            if (versions == NULL)
                return NULL;

            while ((version_name = g_dir_read_name(versions)) != NULL) {
                g_autofree char *version_dir = g_build_filename(component_dir, version_name, NULL);
                g_autoptr(GError) read_error = NULL;
                MfComponent *component;

                if (!mf_is_version_dir_name(version_name) ||
                    g_file_test(version_dir, G_FILE_TEST_IS_SYMLINK) ||
                    !g_file_test(version_dir, G_FILE_TEST_IS_DIR))
                    continue;
                component = mf_read_version_dir_at(root, SUBDIRS[i], id, version_name, &read_error);
                if (component == NULL) {
                    g_debug("Ignoring %s: %s", version_dir, read_error->message);
                    continue;
                }
                g_ptr_array_add(components, component);
            }
        }
    }

    return g_steal_pointer(&components);
}

/*
 * The cache
 */

/**
 * mf_store_load:
 * @store: an #MfStore
 * @error: a #GError
 *
 * Read all installed components, replacing what was read before.
 * This method is run implicitly, but can be called explicitly to
 * refresh the information.
 *
 * Returns: %TRUE on success
 */
gboolean
mf_store_load(MfStore *store, GError **error)
{
    MfStorePrivate *priv;
    GPtrArray *components;

    g_return_val_if_fail(MF_IS_STORE(store), FALSE);

    priv = GET_PRIVATE(store);
    components = mf_scan_at(priv->path, error);
    if (components == NULL)
        return FALSE;
    g_clear_pointer(&priv->components, g_ptr_array_unref);
    priv->components = components;
    return TRUE;
}

/**
 * mf_store_invalidate:
 * @store: an #MfStore
 *
 * Forget what was read, so that the next method that needs it loads the
 * store again.
 */
void
mf_store_invalidate(MfStore *store)
{
    g_clear_pointer(&GET_PRIVATE(store)->components, g_ptr_array_unref);
}

/**
 * mf_store_ensure_loaded:
 * @store: an #MfStore
 * @error: a #GError
 *
 * Load the store, unless it is loaded already.
 *
 * Returns: (transfer none): the installed components, or %NULL on error
 */
static GPtrArray *
mf_store_ensure_loaded(MfStore *store, GError **error)
{
    MfStorePrivate *priv = GET_PRIVATE(store);

    if (priv->components == NULL && !mf_store_load(store, error))
        return NULL;
    return priv->components;
}

/**
 * mf_store_list_components:
 * @store: an #MfStore
 * @component_id: (nullable): only list the versions of this component
 * @kind: only list components of this kind, or %MF_COMPONENT_KIND_UNKNOWN for all
 * @error: a #GError
 *
 * List every installed version of the filtered component.
 *
 * Returns: (transfer full) (element-type MfComponent): the components
 */
GPtrArray *
mf_store_list_components(MfStore *store, const char *component_id, MfComponentKind kind, GError **error)
{
    GPtrArray *components;
    g_autoptr(GPtrArray) result = NULL;

    g_return_val_if_fail(MF_IS_STORE(store), NULL);
    g_return_val_if_fail(component_id == NULL || mf_component_id_is_valid(component_id), NULL);

    components = mf_store_ensure_loaded(store, error);
    if (components == NULL)
        return NULL;

    result = g_ptr_array_new_with_free_func(g_object_unref);
    for (guint i = 0; i < components->len; i++) {
        MfComponent *component = g_ptr_array_index(components, i);

        if (component_id != NULL && !g_str_equal(mf_component_get_id(component), component_id))
            continue;
        if (kind != MF_COMPONENT_KIND_UNKNOWN && mf_component_get_kind(component) != kind)
            continue;
        g_ptr_array_add(result, g_object_ref(component));
    }

    return g_steal_pointer(&result);
}

/**
 * mf_store_get_component:
 * @store: an #MfStore
 * @component_id: identifier of a component
 * @version: (nullable): the version wanted, or %NULL for the current one
 * @error: a #GError
 *
 * Find an installed version of a component: the one the install daemon
 * made current, or an exact version, in its newest revision.
 *
 * Returns: (transfer full): the component, or %NULL on error
 */
MfComponent *
mf_store_get_component(MfStore *store, const char *component_id, const char *version, GError **error)
{
    GPtrArray *components;
    MfComponent *found = NULL;
    gboolean installed = FALSE;

    g_return_val_if_fail(MF_IS_STORE(store), NULL);
    g_return_val_if_fail(mf_component_id_is_valid(component_id), NULL);
    g_return_val_if_fail(version == NULL || mf_version_is_valid(version), NULL);

    components = mf_store_ensure_loaded(store, error);
    if (components == NULL)
        return NULL;

    for (guint i = 0; i < components->len; i++) {
        MfComponent *component = g_ptr_array_index(components, i);

        if (!g_str_equal(mf_component_get_id(component), component_id))
            continue;
        installed = TRUE;
        if (version == NULL ? !mf_component_is_current(component)
                            : !g_str_equal(mf_component_get_version(component), version))
            continue;
        if (found == NULL || mf_component_get_revision(component) > mf_component_get_revision(found))
            found = component;
    }

    if (found != NULL)
        return g_object_ref(found);
    if (!installed)
        g_set_error(error, MF_ERROR, MF_ERROR_NOT_FOUND, "%s is not installed", component_id);
    else if (version == NULL)
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_NOT_FOUND,
                    "%s is installed, but has no current version",
                    component_id);
    else
        g_set_error(error, MF_ERROR, MF_ERROR_NOT_FOUND, "%s %s is not installed", component_id, version);
    return NULL;
}

/**
 * mf_store_get_base:
 * @store: an #MfStore
 * @component: an installed component
 * @error: a #GError
 *
 * Follow the `base` link of a component to the layer directly below it:
 * the version of that component the link names, which is its current one
 * unless the component needs one exact version.
 *
 * Returns: (transfer full): the base, or %NULL if the component has none
 */
MfComponent *
mf_store_get_base(MfStore *store, MfComponent *component, GError **error)
{
    g_auto(GStrv) parts = NULL;

    g_return_val_if_fail(MF_IS_STORE(store), NULL);
    g_return_val_if_fail(MF_IS_COMPONENT(component), NULL);

    parts = mf_store_read_base_link_at(GET_PRIVATE(store)->path, component, error);
    if (parts == NULL)
        return NULL;
    if (g_str_equal(parts[2], MF_STORE_CURRENT_NAME))
        return mf_store_get_component(store, parts[1], NULL, error);

    GPtrArray *components = mf_store_ensure_loaded(store, error);
    if (components == NULL)
        return NULL;
    for (guint i = 0; i < components->len; i++) {
        MfComponent *candidate = g_ptr_array_index(components, i);
        g_autofree char *version_name = NULL;

        if (!g_str_equal(mf_component_get_id(candidate), parts[1]) ||
            !g_str_equal(
                mf_store_subdir_for_kind(mf_component_get_kind(candidate), mf_component_is_sdk(candidate)),
                parts[0]))
            continue;
        version_name = mf_component_make_version_dir_name(candidate);
        if (g_str_equal(version_name, parts[2]))
            return g_object_ref(candidate);
    }

    g_set_error(error,
                MF_ERROR,
                MF_ERROR_NOT_FOUND,
                "The base of %s, %s %s, is not installed",
                mf_component_get_id(component),
                parts[1],
                parts[2]);
    return NULL;
}

/**
 * mf_store_find_identity:
 * @store: an #MfStore
 * @identity: the identity wanted
 * @error: a #GError
 *
 * Find what provides an identity: the current version of a component, if
 * it provides it. Otherwise we return the newest version that still provides
 * the requested identity.
 *
 * Returns: (transfer full) (nullable): the installed core, platform, kit,
 *   SDK or application that provides an identity, and whose image is there;
 *   or %NULL with %MF_ERROR_NOT_FOUND if there is none
 */
MfComponent *
mf_store_find_identity(MfStore *store, const char *identity, GError **error)
{
    GPtrArray *components = mf_store_ensure_loaded(store, error);
    MfComponent *found = NULL;

    if (components == NULL)
        return NULL;

    for (guint i = 0; i < components->len; i++) {
        MfComponent *component = g_ptr_array_index(components, i);
        int cmp;

        if (g_strcmp0(mf_component_get_identity(component), identity) != 0 ||
            !mf_store_has_image(store, mf_component_get_verity_hash(component)))
            continue;
        if (mf_component_is_current(component))
            return g_object_ref(component);
        cmp = found == NULL
                  ? 1
                  : mf_version_compare(mf_component_get_version(component), mf_component_get_version(found));
        if (cmp > 0 || (cmp == 0 && mf_component_get_revision(component) > mf_component_get_revision(found)))
            found = component;
    }

    if (found == NULL) {
        g_set_error(error, MF_ERROR, MF_ERROR_NOT_FOUND, "Nothing installed provides identity %s", identity);
        return NULL;
    }
    return g_object_ref(found);
}

/**
 * mf_store_get_component_dir:
 * @store: an #MfStore
 * @component: an installed component
 *
 * Returns: (transfer full): the directory of the version in the store
 */
char *
mf_store_get_component_dir(MfStore *store, MfComponent *component)
{
    g_return_val_if_fail(MF_IS_STORE(store), NULL);
    g_return_val_if_fail(MF_IS_COMPONENT(component), NULL);

    return mf_component_dir_at(GET_PRIVATE(store)->path, component);
}

/**
 * mf_store_get_image_path:
 * @store: an #MfStore
 * @component: an installed component
 *
 * Returns: (transfer full): absolute path to the image of the component in the store, or %NULL
 */
char *
mf_store_get_image_path(MfStore *store, MfComponent *component)
{
    g_return_val_if_fail(MF_IS_STORE(store), NULL);
    g_return_val_if_fail(MF_IS_COMPONENT(component), NULL);

    return mf_store_image_path_at(GET_PRIVATE(store)->path, mf_component_get_verity_hash(component));
}

/**
 * mf_store_get_manifest:
 * @store: an #MfStore
 * @component: an installed component of @store
 * @error: a #GError
 *
 * Get the manifest a component was installed with, which describes it in
 * full. A component read from the store carries it already; for any
 * other, such as one the install daemon reported, it is read from the
 * component's version directory.
 *
 * Returns: (transfer full): the manifest, or %NULL on error
 */
MfManifest *
mf_store_get_manifest(MfStore *store, MfComponent *component, GError **error)
{
    g_autofree char *version_dir = NULL;
    MfManifest *manifest;

    g_return_val_if_fail(MF_IS_STORE(store), NULL);
    g_return_val_if_fail(MF_IS_COMPONENT(component), NULL);

    manifest = mf_component_peek_manifest(component);
    if (manifest != NULL)
        return g_object_ref(manifest);

    version_dir = mf_component_dir_at(GET_PRIVATE(store)->path, component);
    if (!g_file_test(version_dir, G_FILE_TEST_IS_DIR)) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_NOT_FOUND,
                    "%s %s is not installed",
                    mf_component_get_id(component),
                    mf_component_get_version(component));
        return NULL;
    }
    manifest = mf_read_manifest_at(version_dir,
                                   mf_component_get_id(component),
                                   mf_component_get_version(component),
                                   error);
    if (manifest == NULL)
        return NULL;
    mf_component_set_manifest(component, manifest);
    return manifest;
}

/**
 * mf_store_has_image:
 * @store: an #MfStore
 * @verity_hash: the root hash of an image
 *
 * Returns: whether the image with that root hash is in the store
 */
gboolean
mf_store_has_image(MfStore *store, const char *verity_hash)
{
    g_autofree char *path = mf_store_image_path_at(GET_PRIVATE(store)->path, verity_hash);

    return g_file_test(path, G_FILE_TEST_IS_REGULAR);
}
