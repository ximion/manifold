/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mfd-manager.h"

#include <sys/stat.h>

#include "mf-component-private.h"
#include "mf-result-private.h"
#include "mf-store-private.h"
#include "mf-progress-private.h"
#include "mf-utils-private.h"

#include "mfd-store-writer.h"

/**
 * MfdManager:
 *
 * Installs, updates and removes components in the local store.
 *
 * The manager decides what an operation does, refuses what can not be
 * done before anything is written, and has the #MfdStoreWriter carry it
 * out. Progress is reported through a #MfProgressFunc, and what was done
 * is returned as #MfResult.
 *
 * Any permission checks happen in the caller.
 */
struct _MfdManager {
    GObject parent_instance;

    MfStore *store;
    MfdStoreWriter *writer;
};

G_DEFINE_TYPE(MfdManager, mfd_manager, G_TYPE_OBJECT)

static void
mfd_manager_finalize(GObject *object)
{
    MfdManager *manager = MFD_MANAGER(object);

    g_clear_object(&manager->writer);
    g_clear_object(&manager->store);

    G_OBJECT_CLASS(mfd_manager_parent_class)->finalize(object);
}

static void
mfd_manager_class_init(MfdManagerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = mfd_manager_finalize;
}

static void
mfd_manager_init(MfdManager *manager)
{
}

/**
 * mfd_manager_new_for_store:
 * @store: the store to manage
 *
 * Returns: (transfer full): a new #MfdManager
 */
MfdManager *
mfd_manager_new_for_store(MfStore *store)
{
    MfdManager *manager = g_object_new(MFD_TYPE_MANAGER, NULL);

    manager->store = g_object_ref(store);
    manager->writer = mfd_store_writer_new(store);
    return manager;
}

/**
 * mfd_manager_new:
 *
 * Returns: (transfer full): a new #MfdManager for the store of the system
 */
MfdManager *
mfd_manager_new(void)
{
    g_autoptr(MfStore) store = mf_store_new();

    return mfd_manager_new_for_store(store);
}

/**
 * mfd_error_detail_clear:
 */
void
mfd_error_detail_clear(MfdErrorDetail *detail)
{
    g_clear_pointer(&detail->component_id, g_free);
    g_clear_pointer(&detail->version, g_free);
    g_clear_pointer(&detail->base_id, g_free);
    g_clear_pointer(&detail->base_version, g_free);
    g_clear_pointer(&detail->identity, g_free);
    g_clear_pointer(&detail->used_by, g_free);
    g_clear_pointer(&detail->architecture, g_free);
}

/**
 * MfdProgressTracker:
 *
 * State of the progress of the current operation: the #MfProgress that is
 * reported, and what it is calculated from.
 */
typedef struct {
    MfProgressFunc func;
    gpointer user_data;
    MfProgress *progress;
    MfItemProgress *item; /* the item being worked on, owned by @progress */
    guint total;          /* items the operation consists of */
    guint done;           /* items finished */
    guint64 bytes_last_reported;
    gboolean item_done;
} MfdProgressTracker;

/**
 * mfd_progress_tracker_clear:
 */
static void
mfd_progress_tracker_clear(MfdProgressTracker *tracker)
{
    g_clear_pointer(&tracker->progress, mf_progress_free);
    tracker->item = NULL;
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(MfdProgressTracker, mfd_progress_tracker_clear)

/* how many bytes of an image we copy between two reports of the progress */
constexpr guint64 MFD_PROGRESS_STEP = 4 * 1024 * 1024;

/**
 * mfd_progress_report:
 *
 * Send the state of the operation to the caller.
 */
static void
mfd_progress_report(MfdProgressTracker *tracker)
{
    double item_percent = 0;

    if (tracker->func == NULL)
        return;

    if (tracker->item != NULL) {
        guint64 bytes_done = mf_item_progress_get_bytes_done(tracker->item);
        guint64 bytes_total = mf_item_progress_get_bytes_total(tracker->item);

        if (bytes_total > 0)
            item_percent = (double) bytes_done * 100 / (double) bytes_total;
        else if (tracker->item_done)
            item_percent = 100;
        mf_item_progress_set_percent(tracker->item, item_percent);
        tracker->bytes_last_reported = bytes_done;
    }
    double percent = tracker->total > 0
                         ? ((double) tracker->done * 100 + item_percent) / (double) tracker->total
                         : 100;
    mf_progress_set_percent(tracker->progress, MIN(percent, 100));

    tracker->func(tracker->progress, tracker->user_data);
}

/**
 * mfd_progress_begin_item:
 * @version: (nullable): the version of the component the item is about
 * @bytes_total: the size of the image the item copies, or 0
 *
 * Report that work on an item begins.
 */
static void
mfd_progress_begin_item(MfdProgressTracker *tracker,
                        MfAction action,
                        const char *subject,
                        const char *version,
                        guint64 bytes_total)
{
    tracker->item = mf_item_progress_new(action, subject, version);
    mf_item_progress_set_bytes_total(tracker->item, bytes_total);
    mf_progress_take_item(tracker->progress, tracker->item);
    tracker->bytes_last_reported = 0;
    tracker->item_done = FALSE;
    mfd_progress_report(tracker);
}

/**
 * mfd_progress_end_item:
 *
 * Report that the item is done.
 */
static void
mfd_progress_end_item(MfdProgressTracker *tracker)
{
    mf_item_progress_set_bytes_done(tracker->item, mf_item_progress_get_bytes_total(tracker->item));
    tracker->item_done = TRUE;
    mfd_progress_report(tracker);
    mf_progress_take_item(tracker->progress, NULL);
    tracker->item = NULL;
    tracker->done++;
}

/**
 * mfd_on_copy_progress:
 *
 * Report the progress of copying an image, see mfd_store_writer_add_image().
 */
static void
mfd_on_copy_progress(guint64 bytes_copied, gpointer user_data)
{
    MfdProgressTracker *tracker = user_data;

    mf_item_progress_set_bytes_done(tracker->item, bytes_copied);
    /* the last bytes are reported when the item ends */
    if (bytes_copied - tracker->bytes_last_reported < MFD_PROGRESS_STEP ||
        bytes_copied == mf_item_progress_get_bytes_total(tracker->item))
        return;
    mfd_progress_report(tracker);
}

/**
 * MfdInstallPlan:
 *
 * How we install one image of a bundle as a component, once the image
 * passed every check.
 */
typedef struct {
    MfBundleImage *image; /* the image of the bundle, which outlives the plan */
    MfInstallMode mode;
    gboolean skip;      /* installed already, there is nothing to do */
    gboolean add_image; /* the image is not in the store yet */
    MfBaseLink *base;   /* what the component builds on, NULL for a core */
} MfdInstallPlan;

/**
 * mfd_install_plan_clear:
 */
static void
mfd_install_plan_clear(MfdInstallPlan *iplan)
{
    g_clear_pointer(&iplan->base, mf_base_link_free);
}

/**
 * mfd_set_already_installed_error:
 */
static void
mfd_set_already_installed_error(GError **error,
                                MfdErrorDetail *ret_detail,
                                const char *component_id,
                                const char *version)
{
    if (ret_detail != NULL) {
        mfd_error_detail_clear(ret_detail);
        ret_detail->component_id = g_strdup(component_id);
        ret_detail->version = g_strdup(version);
    }
    g_set_error(error,
                MF_ERROR,
                MF_ERROR_ALREADY_INSTALLED,
                "%s %s is installed already",
                component_id,
                version);
}

/**
 * mfd_set_not_installed_error:
 * @version: (nullable): the version, or %NULL for any version of the component
 */
static void
mfd_set_not_installed_error(GError **error,
                            MfdErrorDetail *ret_detail,
                            const char *component_id,
                            const char *version)
{
    if (ret_detail != NULL) {
        mfd_error_detail_clear(ret_detail);
        ret_detail->component_id = g_strdup(component_id);
        ret_detail->version = g_strdup(version);
    }
    g_set_error(error,
                MF_ERROR,
                MF_ERROR_NOT_FOUND,
                "%s%s%s is not installed",
                component_id,
                version != NULL ? " " : "",
                version != NULL ? version : "");
}

/**
 * mfd_set_wrong_architecture_error:
 */
static void
mfd_set_wrong_architecture_error(GError **error,
                                 MfdErrorDetail *ret_detail,
                                 const char *component_id,
                                 const char *architecture)
{
    if (ret_detail != NULL) {
        mfd_error_detail_clear(ret_detail);
        ret_detail->component_id = g_strdup(component_id);
        ret_detail->architecture = g_strdup(architecture);
    }
    g_set_error(error,
                MF_ERROR,
                MF_ERROR_WRONG_ARCHITECTURE,
                "%s is built for %s, but this system is %s",
                component_id,
                architecture,
                mf_get_host_architecture());
}

/**
 * mfd_manager_resolve_dependency:
 * @iplan: the plan whose base to set
 * @ret_kind: (out): the kind of the component that provides the identity
 *
 * Find the component that provides an identity a component needs, in the
 * bundle or in the store, and make it the base of @iplan. If the bundle and
 * the store both have that component, we take the newer of the two versions.
 *
 * The base link follows the `current` version of that component if the
 * version we found will be the current one once the bundle is installed.
 * Otherwise, it names the exact version we found.
 *
 * Returns: %TRUE if we found a component that provides the identity
 */
static gboolean
mfd_manager_resolve_dependency(MfdManager *manager,
                               MfBundle *bundle,
                               const char *identity,
                               MfdInstallPlan *iplan,
                               MfComponentKind *ret_kind)
{
    MfBundleImage *image = mf_bundle_find_image_for_identity(bundle, identity);
    g_autoptr(MfComponent) installed = mf_store_find_identity(manager->store, identity, NULL);
    g_autofree char *version_name = NULL;
    gboolean current;

    if (image != NULL) {
        MfManifest *manifest = mf_bundle_image_get_manifest(image);
        const char *component_id = mf_manifest_get_component_id(manifest);
        const char *version = mf_manifest_get_version(manifest);
        guint64 revision = mf_manifest_get_revision(manifest);

        if (installed == NULL || g_strcmp0(mf_component_get_id(installed), component_id) != 0 ||
            mf_version_compare_with_rev(mf_component_get_version(installed),
                                        mf_component_get_revision(installed),
                                        version,
                                        revision) < 0) {
            /* the bundle's image becomes current unless a newer version is already installed */
            g_autoptr(MfComponent) newest = mf_store_get_component(manager->store, component_id, NULL, NULL);

            current = newest == NULL || mf_version_compare_with_rev(version,
                                                                    revision,
                                                                    mf_component_get_version(newest),
                                                                    mf_component_get_revision(newest)) >= 0;
            version_name = current ? NULL : mf_build_full_version(version, revision);
            *ret_kind = mf_manifest_get_kind(manifest);
            iplan->base = mf_base_link_new(mf_store_subdir_for_kind(*ret_kind, mf_manifest_is_sdk(manifest)),
                                           component_id,
                                           version_name);
            return TRUE;
        }
    }

    if (installed == NULL)
        return FALSE;
    *ret_kind = mf_component_get_kind(installed);
    if (!mf_component_is_current(installed))
        version_name = mf_build_full_version(mf_component_get_version(installed),
                                             mf_component_get_revision(installed));
    iplan->base = mf_base_link_new(mf_store_subdir_for_kind(*ret_kind, mf_component_is_sdk(installed)),
                                   mf_component_get_id(installed),
                                   version_name);
    return TRUE;
}

/**
 * mfd_bundle_is_dependency_of_app:
 *
 * A kit that arrives in the same bundle as an application that runs on it
 * was not installed on purpose, it just came along.
 */
static gboolean
mfd_bundle_is_dependency_of_app(MfBundle *bundle, MfManifest *manifest)
{
    GPtrArray *images = mf_bundle_get_images(bundle);

    if (mf_manifest_get_kind(manifest) != MF_COMPONENT_KIND_KIT || mf_manifest_is_sdk(manifest))
        return FALSE;

    for (guint i = 0; i < images->len; i++) {
        MfManifest *app = mf_bundle_image_get_manifest(g_ptr_array_index(images, i));

        if (mf_manifest_get_kind(app) == MF_COMPONENT_KIND_APP && !mf_manifest_is_sdk(app) &&
            g_strcmp0(mf_manifest_get_base_identity(app), mf_manifest_get_identity(manifest)) == 0)
            return TRUE;
    }

    return FALSE;
}

/**
 * mfd_manager_get_installed:
 *
 * Returns: (transfer full) (nullable): the installed revision of a version of a component
 */
static MfComponent *
mfd_manager_get_installed(MfdManager *manager,
                          const char *component_id,
                          const char *version,
                          guint64 revision)
{
    g_autoptr(GPtrArray) versions = NULL;

    versions = mf_store_list_components(manager->store, component_id, MF_COMPONENT_KIND_UNKNOWN, NULL);
    for (guint i = 0; versions != NULL && i < versions->len; i++) {
        MfComponent *component = g_ptr_array_index(versions, i);

        if (g_str_equal(mf_component_get_version(component), version) &&
            mf_component_get_revision(component) == revision)
            return g_object_ref(component);
    }
    return NULL;
}

/**
 * mfd_manager_plan_image_install:
 *
 * Plan how we install an image of the bundle: whether we add the image to
 * the store, whether the component is installed already, its install mode,
 * and what it builds on. Everything that refuses the installation of the
 * image is checked here, before anything is written.
 *
 * Returns: %TRUE if the image can be installed
 */
static gboolean
mfd_manager_plan_image_install(MfdManager *manager,
                               MfBundle *bundle,
                               MfBundleImage *image,
                               MfdInstallPlan *iplan,
                               MfdErrorDetail *ret_detail,
                               GError **error)
{
    MfManifest *manifest = mf_bundle_image_get_manifest(image);
    const char *component_id = mf_manifest_get_component_id(manifest);
    const char *version = mf_manifest_get_version(manifest);
    const char *verity_hash = mf_manifest_get_verity_hash(manifest);
    const char *base_id = mf_manifest_get_base_id(manifest);
    MfComponentKind kind = mf_manifest_get_kind(manifest);
    gboolean sdk = mf_manifest_is_sdk(manifest);
    gboolean dependency = mfd_bundle_is_dependency_of_app(bundle, manifest);
    g_autoptr(MfComponent) existing = NULL;

    if (!g_str_equal(mf_manifest_get_architecture(manifest), mf_get_host_architecture())) {
        mfd_set_wrong_architecture_error(error,
                                         ret_detail,
                                         component_id,
                                         mf_manifest_get_architecture(manifest));
        return FALSE;
    }

    iplan->image = image;
    iplan->add_image = !mf_store_has_image(manager->store, verity_hash);
    iplan->mode = dependency || mf_component_kind_is_always_auto(kind, sdk) ? MF_INSTALL_MODE_AUTO
                                                                            : MF_INSTALL_MODE_MANUAL;

    existing = mfd_manager_get_installed(manager, component_id, version, mf_manifest_get_revision(manifest));
    if (existing != NULL) {
        if (g_strcmp0(mf_component_get_verity_hash(existing), verity_hash) != 0) {
            g_warning("%s %s is installed already, with another image: %s instead of %s",
                      component_id,
                      version,
                      mf_component_get_verity_hash(existing),
                      verity_hash);
            mfd_set_already_installed_error(error, ret_detail, component_id, version);
            return FALSE;
        }
        if (dependency) {
            /* already installed (how it got there doesn't matter) */
            iplan->skip = TRUE;
            return TRUE;
        }
        /* only a runtime that came along as a dependency can be installed on purpose again */
        if (mf_component_kind_is_always_auto(kind, sdk) ||
            mf_component_get_install_mode(existing) != MF_INSTALL_MODE_AUTO) {
            mfd_set_already_installed_error(error, ret_detail, component_id, version);
            return FALSE;
        }
        g_info("%s %s was a dependency, and is installed on purpose now", component_id, version);
    }

    if (base_id != NULL) {
        const char *base_identity = mf_manifest_get_base_identity(manifest);
        MfComponentKind base_kind;

        if (!mfd_manager_resolve_dependency(manager, bundle, base_identity, iplan, &base_kind)) {
            if (ret_detail != NULL) {
                mfd_error_detail_clear(ret_detail);
                ret_detail->component_id = g_strdup(component_id);
                ret_detail->base_id = g_strdup(base_id);
                ret_detail->base_version = g_strdup(mf_manifest_get_base_version(manifest));
                ret_detail->identity = g_strdup(base_identity);
            }
            g_set_error(error,
                        MF_ERROR,
                        MF_ERROR_MISSING_BASE,
                        "%s needs base %s %s (identity %s), which is not installed",
                        component_id,
                        base_id,
                        mf_manifest_get_base_version(manifest),
                        base_identity);
            return FALSE;
        }

        /* a platform is composed with one exact core, and nothing else below it */
        if (kind == MF_COMPONENT_KIND_PLATFORM && !sdk && base_kind != MF_COMPONENT_KIND_CORE) {
            g_set_error(error,
                        MF_ERROR,
                        MF_ERROR_INVALID_BUNDLE,
                        "%s: the base of a platform must be a core, not the %s %s",
                        mf_bundle_image_get_manifest_name(image),
                        mf_component_kind_to_string(base_kind),
                        base_id);
            return FALSE;
        }

        /* the SDK kind must match its base kind */
        if (sdk && kind != base_kind) {
            g_set_error(error,
                        MF_ERROR,
                        MF_ERROR_INVALID_BUNDLE,
                        "%s: the base of an SDK must be of the same type (expected %s, got %s)",
                        mf_bundle_image_get_manifest_name(image),
                        mf_component_kind_to_string(kind),
                        mf_component_kind_to_string(base_kind));
            return FALSE;
        }
    }

    /* the layer directly below is what provides the base identity, see
     * mfd_manager_resolve_dependency(). A platform and an SDK need the exact
     * version they were built against */
    if (base_id != NULL && (sdk || kind == MF_COMPONENT_KIND_PLATFORM)) {
        g_autofree char *version_name = mf_build_full_version(mf_manifest_get_base_version(manifest),
                                                              mf_manifest_get_base_revision(manifest));
        MfBaseLink *pinned = mf_base_link_new(iplan->base->subdir, base_id, version_name);

        mf_base_link_free(iplan->base);
        iplan->base = pinned;
    }

    return TRUE;
}

/**
 * mfd_removal_item_free:
 */
static void
mfd_removal_item_free(MfdRemovalItem *item)
{
    g_object_unref(item->component);
    g_free(item->version_dir);
    g_free(item);
}

/**
 * mfd_removal_plan_free:
 */
void
mfd_removal_plan_free(MfdRemovalPlan *rmplan)
{
    if (rmplan == NULL)
        return;

    g_hash_table_unref(rmplan->item_dirs);
    g_ptr_array_unref(rmplan->items);
    g_ptr_array_unref(rmplan->images);
    g_free(rmplan);
}

/**
 * mfd_removal_plan_new:
 *
 * Returns: (transfer full): a plan that removes nothing yet
 */
static MfdRemovalPlan *
mfd_removal_plan_new(void)
{
    MfdRemovalPlan *rmplan = g_new0(MfdRemovalPlan, 1);

    rmplan->items = g_ptr_array_new_with_free_func((GDestroyNotify) mfd_removal_item_free);
    /* the set holds the version directories of the items, which own them */
    rmplan->item_dirs = g_hash_table_new(g_str_hash, g_str_equal);
    rmplan->images = g_ptr_array_new_with_free_func(g_free);
    return rmplan;
}

/**
 * MfdInstalledVersion:
 * @component: an installed version of a component
 * @version_dir: its version directory
 *
 * What removals are planned against.
 */
typedef struct {
    MfComponent *component;
    char *version_dir;
} MfdInstalledVersion;

/**
 * mfd_installed_version_free:
 */
static void
mfd_installed_version_free(MfdInstalledVersion *version)
{
    g_object_unref(version->component);
    g_free(version->version_dir);
    g_free(version);
}

/**
 * mfd_manager_list_installed:
 *
 * List every installed version, reading every manifest once.
 *
 * Returns: (transfer full) (element-type MfdInstalledVersion): the versions, or %NULL on error
 */
static GPtrArray *
mfd_manager_list_installed(MfdManager *manager, GError **error)
{
    g_autoptr(GPtrArray) components = NULL;
    GPtrArray *installed;

    components = mf_store_list_components(manager->store, NULL, MF_COMPONENT_KIND_UNKNOWN, error);
    if (components == NULL)
        return NULL;

    installed = g_ptr_array_new_with_free_func((GDestroyNotify) mfd_installed_version_free);
    for (guint i = 0; i < components->len; i++) {
        MfdInstalledVersion *version = g_new0(MfdInstalledVersion, 1);

        version->component = g_object_ref(g_ptr_array_index(components, i));
        version->version_dir = mf_store_get_component_dir(manager->store, version->component);
        g_ptr_array_add(installed, version);
    }

    return installed;
}

/**
 * mfd_removal_plan_add_item:
 * @rmplan: the #MfdRemovalPlan
 * @version: the version that goes
 * @action: why it goes, see #MfdRemovalItem
 *
 * Add a component to a removal plan.
 */
static void
mfd_removal_plan_add_item(MfdRemovalPlan *rmplan, const MfdInstalledVersion *version, MfAction action)
{
    MfdRemovalItem *item = g_new0(MfdRemovalItem, 1);

    item->component = g_object_ref(version->component);
    item->version_dir = g_strdup(version->version_dir);
    item->action = action;
    g_ptr_array_add(rmplan->items, item);
    g_hash_table_add(rmplan->item_dirs, item->version_dir);
}

/**
 * mfd_manager_find_used_dirs:
 * @installed: (element-type MfdInstalledVersion): what is installed
 * @rm_pending: version directories whose links do not count
 *
 * Find what the `base` links of the versions that stay refer to. A link
 * to `current` refers to the version `current` names, so nothing gets
 * downgraded under it. A base that can not be read is not protected.
 *
 * Returns: a table of every version directory a link names, mapped to the component-ID that links to it
 */
static GHashTable *
mfd_manager_find_used_dirs(MfdManager *manager, GPtrArray *installed, GHashTable *rm_pending)
{
    const char *root = mf_store_get_path(manager->store);
    GHashTable *used = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    for (guint i = 0; i < installed->len; i++) {
        MfdInstalledVersion *version = g_ptr_array_index(installed, i);
        g_autoptr(MfBaseLink) base = NULL;
        g_autofree char *component_dir = NULL;
        g_autofree char *version_name = NULL;

        if (g_hash_table_contains(rm_pending, version->version_dir))
            continue;
        base = mf_store_read_base_link_at(root, version->component, NULL);
        if (base == NULL)
            continue;
        component_dir = g_build_filename(root, base->subdir, base->component_id, NULL);
        version_name = g_strdup(base->version_name);
        if (version_name == NULL) {
            /* a link to `current` keeps the version it names, to prevent accidental downgrades */
            g_autofree char *current_path = g_build_filename(component_dir, MF_STORE_CURRENT_NAME, NULL);

            version_name = g_file_read_link(current_path, NULL);
            if (version_name == NULL)
                continue;
        }
        g_hash_table_insert(used,
                            g_build_filename(component_dir, version_name, NULL),
                            (gpointer) mf_component_get_id(version->component));
    }

    return used;
}

/**
 * mfd_find_used_images:
 * @installed: (element-type MfdInstalledVersion): what is installed
 * @rm_pending: version directories whose links do not count
 *
 * Returns: a set of the root hash of every image that is (or will be) still in use
 */
static GHashTable *
mfd_find_used_images(GPtrArray *installed, GHashTable *rm_pending)
{
    GHashTable *used = g_hash_table_new(g_str_hash, g_str_equal);

    for (guint i = 0; i < installed->len; i++) {
        MfdInstalledVersion *version = g_ptr_array_index(installed, i);

        if (!g_hash_table_contains(rm_pending, version->version_dir))
            g_hash_table_add(used, (gpointer) mf_component_get_verity_hash(version->component));
    }

    return used;
}

/**
 * mfd_find_newest_apps:
 * @installed: (element-type MfdInstalledVersion): what is installed
 * @rm_pending: version directories that go
 *
 * Find the newest version of every application and plugin that stays,
 * which is kept whatever its install mode is.
 *
 * Returns: a set of their version directories
 */
static GHashTable *
mfd_find_newest_apps(GPtrArray *installed, GHashTable *rm_pending)
{
    /* the component-ID mapped to its newest version */
    g_autoptr(GHashTable) newest = g_hash_table_new(g_str_hash, g_str_equal);
    GHashTable *kept = g_hash_table_new(g_str_hash, g_str_equal);
    GHashTableIter iter;
    gpointer value;

    for (guint i = 0; i < installed->len; i++) {
        MfdInstalledVersion *version = g_ptr_array_index(installed, i);
        MfComponent *component = version->component;
        MfdInstalledVersion *other;

        if (!mf_component_kind_is_always_auto(mf_component_get_kind(component),
                                              mf_component_is_sdk(component)) ||
            g_hash_table_contains(rm_pending, version->version_dir))
            continue;
        other = g_hash_table_lookup(newest, mf_component_get_id(component));
        if (other == NULL || mf_version_compare_with_rev(mf_component_get_version(component),
                                                         mf_component_get_revision(component),
                                                         mf_component_get_version(other->component),
                                                         mf_component_get_revision(other->component)) > 0)
            g_hash_table_insert(newest, (gpointer) mf_component_get_id(component), version);
    }

    g_hash_table_iter_init(&iter, newest);
    while (g_hash_table_iter_next(&iter, NULL, &value))
        g_hash_table_add(kept, ((MfdInstalledVersion *) value)->version_dir);

    return kept;
}

/**
 * mfd_manager_complete_removal_plan:
 * @installed: (element-type MfdInstalledVersion): what is installed
 *
 * Add every dependency nothing builds on once the planned components are
 * gone to a removal, and then every image no remaining component links to.
 */
static gboolean
mfd_manager_complete_removal_plan(MfdManager *manager,
                                  MfdRemovalPlan *rmplan,
                                  GPtrArray *installed,
                                  GError **error)
{
    g_autoptr(GHashTable) newest_apps = NULL;
    g_autoptr(GHashTable) images_used = NULL;
    g_autoptr(GDir) images = NULL;
    g_autofree char *images_dir = NULL;
    gboolean added;
    const char *name;

    /* check for any cruft that we can drop now that nothing would depend on it anymore,
     * but keep the newest version of every application and plugin. What goes may have
     * been all that kept something else, so this is repeated until nothing more goes */
    newest_apps = mfd_find_newest_apps(installed, rmplan->item_dirs);
    do {
        g_autoptr(GHashTable) used = mfd_manager_find_used_dirs(manager, installed, rmplan->item_dirs);

        added = FALSE;
        for (guint i = 0; i < installed->len; i++) {
            MfdInstalledVersion *version = g_ptr_array_index(installed, i);

            if (mf_component_get_install_mode(version->component) != MF_INSTALL_MODE_AUTO ||
                g_hash_table_contains(rmplan->item_dirs, version->version_dir) ||
                g_hash_table_contains(used, version->version_dir) ||
                g_hash_table_contains(newest_apps, version->version_dir))
                continue;
            g_info("%s is not needed anymore", version->version_dir);
            mfd_removal_plan_add_item(rmplan, version, MF_ACTION_CLEANUP);
            added = TRUE;
        }
    } while (added);

    /* the images nothing links to, once the components are gone */
    images_used = mfd_find_used_images(installed, rmplan->item_dirs);
    images_dir = g_build_filename(mf_store_get_path(manager->store), MF_STORE_IMAGES_SUBDIR, NULL);
    images = g_dir_open(images_dir, 0, error);
    if (images == NULL)
        return FALSE;
    while ((name = g_dir_read_name(images)) != NULL) {
        g_autofree char *hash = mf_store_hash_from_image_name(name);
        g_autofree char *path = NULL;
        struct stat st;

        if (hash == NULL || g_hash_table_contains(images_used, hash))
            continue;
        path = mf_store_image_path_at(mf_store_get_path(manager->store), hash);
        if (stat(path, &st) == 0)
            rmplan->image_bytes += (guint64) st.st_size;
        g_ptr_array_add(rmplan->images, g_steal_pointer(&hash));
    }

    return TRUE;
}

/**
 * mfd_manager_plan_removal:
 * @manager: an #MfdManager
 * @component_id: (nullable): a component to remove, or %NULL to only clean up cruft
 * @version: (nullable): the version to remove, or %NULL for all versions
 * @ret_detail: (out caller-allocates) (optional): more about a failure
 * @error: a #GError
 *
 * Work out what a removal removes: the versions of the component, every
 * dependency nothing builds on once they are gone, and then every image
 * no remaining component links to. Nothing is changed.
 *
 * Returns: (transfer full): the plan, or %NULL on error
 */
MfdRemovalPlan *
mfd_manager_plan_removal(MfdManager *manager,
                         const char *component_id,
                         const char *version,
                         MfdErrorDetail *ret_detail,
                         GError **error)
{
    g_autoptr(MfdRemovalPlan) rmplan = mfd_removal_plan_new();
    g_autoptr(GPtrArray) installed = NULL;
    g_autoptr(GHashTable) used = NULL;

    g_return_val_if_fail(MFD_IS_MANAGER(manager), NULL);

    installed = mfd_manager_list_installed(manager, error);
    if (installed == NULL)
        return NULL;

    if (component_id != NULL) {
        for (guint i = 0; i < installed->len; i++) {
            MfdInstalledVersion *candidate = g_ptr_array_index(installed, i);

            if (g_str_equal(mf_component_get_id(candidate->component), component_id) &&
                (version == NULL || g_str_equal(mf_component_get_version(candidate->component), version)))
                mfd_removal_plan_add_item(rmplan, candidate, MF_ACTION_REMOVE);
        }
        if (rmplan->items->len == 0) {
            mfd_set_not_installed_error(error, ret_detail, component_id, version);
            return NULL;
        }

        /* nothing that stays may link to stuff that is going to be removed */
        used = mfd_manager_find_used_dirs(manager, installed, rmplan->item_dirs);
        for (guint i = 0; i < rmplan->items->len; i++) {
            MfdRemovalItem *item = g_ptr_array_index(rmplan->items, i);
            const char *used_by = g_hash_table_lookup(used, item->version_dir);

            if (used_by != NULL) {
                if (ret_detail != NULL) {
                    mfd_error_detail_clear(ret_detail);
                    ret_detail->component_id = g_strdup(component_id);
                    ret_detail->used_by = g_strdup(used_by);
                }
                g_set_error(error,
                            MF_ERROR,
                            MF_ERROR_COMPONENT_IN_USE,
                            "Component %s is used by %s",
                            component_id,
                            used_by);
                return NULL;
            }
        }
    }

    if (!mfd_manager_complete_removal_plan(manager, rmplan, installed, error))
        return NULL;
    return g_steal_pointer(&rmplan);
}

/**
 * mfd_manager_plan_supersede:
 * @plans: (element-type MfdInstallPlan): the plans of the installation
 *
 * Work out what an installation supersedes: the older versions and revisions
 * of every component it installed, if they are marked as auto and nothing
 * links to them. As with any removal, the plan then also removes every
 * dependency that nothing builds on anymore, and every image that no
 * remaining component uses. We do not change anything yet.
 *
 * Returns: (transfer full): the plan, or %NULL on error
 */
static MfdRemovalPlan *
mfd_manager_plan_supersede(MfdManager *manager, GArray *plans, GError **error)
{
    g_autoptr(MfdRemovalPlan) rmplan = mfd_removal_plan_new();
    g_autoptr(GPtrArray) installed = NULL;
    g_autoptr(GHashTable) superseded = g_hash_table_new(g_str_hash, g_str_equal);
    g_autoptr(GHashTable) used = NULL;
    gboolean kept;

    installed = mfd_manager_list_installed(manager, error);
    if (installed == NULL)
        return NULL;

    for (guint i = 0; i < installed->len; i++) {
        MfdInstalledVersion *version = g_ptr_array_index(installed, i);
        MfComponent *component = version->component;

        if (mf_component_get_install_mode(component) != MF_INSTALL_MODE_AUTO)
            continue;
        for (guint j = 0; j < plans->len; j++) {
            MfdInstallPlan *plan = &g_array_index(plans, MfdInstallPlan, j);
            MfManifest *manifest = mf_bundle_image_get_manifest(plan->image);

            if (!plan->skip &&
                g_str_equal(mf_component_get_id(component), mf_manifest_get_component_id(manifest)) &&
                mf_version_compare_with_rev(mf_component_get_version(component),
                                            mf_component_get_revision(component),
                                            mf_manifest_get_version(manifest),
                                            mf_manifest_get_revision(manifest)) < 0) {
                g_hash_table_add(superseded, version->version_dir);
                break;
            }
        }
    }

    /* we keep a superseded version that a version we keep links to, and repeat that, as the
     * kept version may link to another superseded version in turn */
    do {
        GHashTableIter iter;
        gpointer version_dir;

        g_clear_pointer(&used, g_hash_table_unref);
        used = mfd_manager_find_used_dirs(manager, installed, superseded);
        kept = FALSE;
        g_hash_table_iter_init(&iter, superseded);
        while (g_hash_table_iter_next(&iter, &version_dir, NULL)) {
            const char *used_by = g_hash_table_lookup(used, version_dir);

            if (used_by == NULL)
                continue;
            g_info("%s is superseded, but %s still builds on it", (const char *) version_dir, used_by);
            g_hash_table_iter_remove(&iter);
            kept = TRUE;
        }
    } while (kept);

    for (guint i = 0; i < installed->len; i++) {
        MfdInstalledVersion *version = g_ptr_array_index(installed, i);

        if (g_hash_table_contains(superseded, version->version_dir))
            mfd_removal_plan_add_item(rmplan, version, MF_ACTION_CLEANUP);
    }

    /* an installation that supersedes nothing removes nothing else either */
    if (rmplan->items->len > 0 && !mfd_manager_complete_removal_plan(manager, rmplan, installed, error))
        return NULL;
    return g_steal_pointer(&rmplan);
}

/**
 * mfd_manager_execute_removal:
 * @result: where to add what was removed
 *
 * Remove what a plan describes, first the component directories, then the images.
 * The images of a component are unlinked while they may be in use; their
 * contents exist for as long as something has them mounted.
 *
 * Progress is reported for the components only.
 */
static gboolean
mfd_manager_execute_removal(MfdManager *manager,
                            MfdRemovalPlan *rmplan,
                            MfdProgressTracker *tracker,
                            MfResult *result,
                            GError **error)
{
    for (guint i = 0; i < rmplan->items->len; i++) {
        MfdRemovalItem *item = g_ptr_array_index(rmplan->items, i);

        mfd_progress_begin_item(tracker,
                                item->action,
                                mf_component_get_id(item->component),
                                mf_component_get_version(item->component),
                                0);
        if (!mfd_store_writer_remove_version_dir(manager->writer, item->version_dir, error))
            return FALSE;
        mfd_progress_end_item(tracker);
        mf_result_add_removed(result, item->component);
    }

    for (guint i = 0; i < rmplan->images->len; i++) {
        const char *hash = g_ptr_array_index(rmplan->images, i);

        if (!mfd_store_writer_remove_image(manager->writer, hash, error))
            return FALSE;
        mf_result_add_removed_image(result, hash);
    }
    mf_result_set_bytes_freed(result, rmplan->image_bytes);

    return TRUE;
}

/**
 * mfd_manager_install_bundle:
 * @manager: an #MfdManager
 * @bundle: the bundle to install
 * @progress_func: (nullable): called whenever the operation progresses
 * @user_data: passed to @progress_func
 * @ret_detail: (out caller-allocates) (optional): more about a failure
 * @error: a #GError
 *
 * Install everything a bundle contains. Everything that can be refused is
 * refused before anything is written. Then the older versions and revisions
 * the installation supersedes are removed, see mfd_manager_plan_supersede().
 *
 * Returns: (transfer full): what was installed, or %NULL on error
 */
MfResult *
mfd_manager_install_bundle(MfdManager *manager,
                           MfBundle *bundle,
                           MfProgressFunc progress_func,
                           gpointer user_data,
                           MfdErrorDetail *ret_detail,
                           GError **error)
{
    g_auto(MfdProgressTracker) tracker = {
        .func = progress_func,
        .user_data = user_data,
        .progress = mf_progress_new(),
    };
    g_autoptr(MfResult) result = NULL;
    g_autoptr(GArray) plans = NULL;
    g_autoptr(MfdRemovalPlan) rmplan = NULL;
    g_autoptr(GError) supersede_error = NULL;
    guint64 bytes_added = 0;

    g_return_val_if_fail(MFD_IS_MANAGER(manager), NULL);

    if (!mfd_store_writer_ensure_root(manager->writer, error))
        return NULL;
    /* reload the store, to ensure all our information is up-to-date */
    if (!mf_store_load(manager->store, error))
        return NULL;

    GPtrArray *images = mf_bundle_get_images(bundle);
    plans = g_array_sized_new(FALSE, TRUE, sizeof(MfdInstallPlan), images->len);
    g_array_set_size(plans, images->len);
    g_array_set_clear_func(plans, (GDestroyNotify) mfd_install_plan_clear);
    for (guint i = 0; i < images->len; i++) {
        if (!mfd_manager_plan_image_install(manager,
                                            bundle,
                                            g_ptr_array_index(images, i),
                                            &g_array_index(plans, MfdInstallPlan, i),
                                            ret_detail,
                                            error))
            return NULL;
    }

    /* copy the images first, then add the referrals and metadata */
    for (guint i = 0; i < plans->len; i++) {
        if (g_array_index(plans, MfdInstallPlan, i).add_image)
            tracker.total++;
    }
    for (guint i = 0; i < plans->len; i++) {
        MfdInstallPlan *plan = &g_array_index(plans, MfdInstallPlan, i);
        MfManifest *manifest = mf_bundle_image_get_manifest(plan->image);
        const char *verity_hash = mf_manifest_get_verity_hash(manifest);
        const char *component_id = mf_manifest_get_component_id(manifest);
        const char *version = mf_manifest_get_version(manifest);
        guint64 image_size = mf_manifest_get_image_size(manifest);

        if (!plan->add_image) {
            g_info("Image %s of %s %s is in the store already", verity_hash, component_id, version);
            continue;
        }

        g_info("Adding image %s of %s %s", verity_hash, component_id, version);
        mfd_progress_begin_item(&tracker, MF_ACTION_INSTALL, component_id, version, image_size);
        if (!mfd_store_writer_add_image(manager->writer,
                                        bundle,
                                        plan->image,
                                        mfd_on_copy_progress,
                                        &tracker,
                                        error))
            return NULL;
        mfd_progress_end_item(&tracker);
        bytes_added += image_size;
    }

    for (guint i = 0; i < plans->len; i++) {
        MfdInstallPlan *plan = &g_array_index(plans, MfdInstallPlan, i);
        MfManifest *manifest = mf_bundle_image_get_manifest(plan->image);
        const char *component_id = mf_manifest_get_component_id(manifest);
        const char *version = mf_manifest_get_version(manifest);
        g_autoptr(GError) local_error = NULL;

        if (plan->skip)
            continue;

        if (!mfd_store_writer_install_component(manager->writer,
                                                manifest,
                                                plan->base,
                                                mf_bundle_image_get_metainfo(plan->image),
                                                plan->mode,
                                                &local_error)) {
            if (g_error_matches(local_error, MF_ERROR, MF_ERROR_ALREADY_INSTALLED))
                mfd_set_already_installed_error(error, ret_detail, component_id, version);
            else
                g_propagate_error(error, g_steal_pointer(&local_error));
            return NULL;
        }
    }

    /* what is superseded goes, but the installation stands even if it can not be removed */
    result = mf_result_new();
    rmplan = mfd_manager_plan_supersede(manager, plans, &supersede_error);
    if (rmplan != NULL)
        tracker.total += rmplan->items->len;
    if (rmplan == NULL || !mfd_manager_execute_removal(manager, rmplan, &tracker, result, &supersede_error))
        g_warning("Unable to remove what the installation supersedes: %s", supersede_error->message);

    /* emit the components of the bundle that were installed now as results, not the
     * dependencies that were there already */
    for (guint i = 0; i < plans->len; i++) {
        MfdInstallPlan *plan = &g_array_index(plans, MfdInstallPlan, i);
        MfManifest *manifest = mf_bundle_image_get_manifest(plan->image);
        g_autoptr(MfComponent) component = NULL;

        if (plan->skip)
            continue;
        component = mfd_manager_get_installed(manager,
                                              mf_manifest_get_component_id(manifest),
                                              mf_manifest_get_version(manifest),
                                              mf_manifest_get_revision(manifest));
        if (component == NULL ||
            g_strcmp0(mf_component_get_verity_hash(component), mf_manifest_get_verity_hash(manifest)) != 0)
            continue;
        mf_result_add_installed(result, component);
    }
    mf_result_set_bytes_added(result, bytes_added);

    return g_steal_pointer(&result);
}

/**
 * mfd_manager_remove:
 * @manager: an #MfdManager
 * @component_id: (nullable): the component to remove, or %NULL to only remove cruft
 * @version: (nullable): the version to remove, or %NULL for all versions
 * @progress_func: (nullable): called whenever the operation progresses
 * @user_data: passed to @progress_func
 * @ret_detail: (out caller-allocates) (optional): more about a failure
 * @error: a #GError
 *
 * Remove a component, or one version of it, as long as it's not a dependency of
 * something else.
 *
 * Any cruft that interrupted operations left behind is deleted first, then
 * what the plan says, see mfd_manager_execute_removal().
 *
 * Returns: (transfer full): what was removed, or %NULL on error
 */
MfResult *
mfd_manager_remove(MfdManager *manager,
                   const char *component_id,
                   const char *version,
                   MfProgressFunc progress_func,
                   gpointer user_data,
                   MfdErrorDetail *ret_detail,
                   GError **error)
{
    g_auto(MfdProgressTracker) tracker = {
        .func = progress_func,
        .user_data = user_data,
        .progress = mf_progress_new(),
    };
    g_autoptr(MfdRemovalPlan) rmplan = NULL;
    g_autoptr(MfResult) result = NULL;

    g_return_val_if_fail(MFD_IS_MANAGER(manager), NULL);

    if (!mfd_store_writer_ensure_root(manager->writer, error))
        return NULL;

    /* what is removed is planned against what is installed now */
    if (!mf_store_load(manager->store, error))
        return NULL;
    rmplan = mfd_manager_plan_removal(manager, component_id, version, ret_detail, error);
    if (rmplan == NULL)
        return NULL;

    tracker.total = rmplan->items->len;
    if (!mfd_store_writer_remove_cruft(manager->writer, error))
        return NULL;

    result = mf_result_new();
    if (!mfd_manager_execute_removal(manager, rmplan, &tracker, result, error))
        return NULL;

    return g_steal_pointer(&result);
}

/**
 * mfd_manager_mark_selects:
 *
 * Whether marking @version of a component, or its current version if
 * @version is %NULL, marks @component.
 */
static gboolean
mfd_manager_mark_selects(MfComponent *component, const char *version)
{
    if (version == NULL)
        return mf_component_is_current(component);
    return g_str_equal(mf_component_get_version(component), version);
}

/**
 * mfd_manager_mark:
 * @manager: an #MfdManager
 * @component_id: the component to mark
 * @version: (nullable): the version to mark, with all of its revisions, or %NULL for the current one
 * @mode: the install mode to record
 * @ret_detail: (out caller-allocates) (optional): more about a failure
 * @error: a #GError
 *
 * Mark versions of a component as kept until they are removed explicitly
 * (%MF_INSTALL_MODE_MANUAL), or as removed once they are not needed anymore
 * (%MF_INSTALL_MODE_AUTO). Nothing is removed right away, but by the next
 * installation, removal or cleanup.
 *
 * Returns: (transfer full) (element-type MfComponent): the marked versions, or %NULL on error
 */
GPtrArray *
mfd_manager_mark(MfdManager *manager,
                 const char *component_id,
                 const char *version,
                 MfInstallMode mode,
                 MfdErrorDetail *ret_detail,
                 GError **error)
{
    g_autoptr(GPtrArray) components = NULL;
    g_autoptr(GPtrArray) marked = g_ptr_array_new_with_free_func(g_object_unref);
    guint n_marked = 0;

    g_return_val_if_fail(MFD_IS_MANAGER(manager), NULL);
    g_return_val_if_fail(mode == MF_INSTALL_MODE_MANUAL || mode == MF_INSTALL_MODE_AUTO, NULL);

    if (!mf_store_load(manager->store, error))
        return NULL;
    components = mf_store_list_components(manager->store, component_id, MF_COMPONENT_KIND_UNKNOWN, error);
    if (components == NULL)
        return NULL;

    for (guint i = 0; i < components->len; i++) {
        MfComponent *component = g_ptr_array_index(components, i);
        g_autofree char *version_dir = NULL;

        if (!mfd_manager_mark_selects(component, version))
            continue;
        version_dir = mf_store_get_component_dir(manager->store, component);
        if (!mfd_store_writer_set_mode(manager->writer, version_dir, mode, error))
            return NULL;
        g_info("Marked %s as %s", version_dir, mf_install_mode_to_string(mode));
        n_marked++;
    }
    if (n_marked == 0) {
        mfd_set_not_installed_error(error, ret_detail, component_id, version);
        return NULL;
    }

    /* what was marked, as it is now */
    g_clear_pointer(&components, g_ptr_array_unref);
    components = mf_store_list_components(manager->store, component_id, MF_COMPONENT_KIND_UNKNOWN, error);
    if (components == NULL)
        return NULL;
    for (guint i = 0; i < components->len; i++) {
        MfComponent *component = g_ptr_array_index(components, i);

        if (mfd_manager_mark_selects(component, version))
            g_ptr_array_add(marked, g_object_ref(component));
    }

    return g_steal_pointer(&marked);
}
