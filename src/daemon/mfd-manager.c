/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mfd-manager.h"

#include <sys/stat.h>

#include "mf-result-private.h"
#include "mf-store-private.h"
#include "mf-progress-private.h"

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
 * Verified plan to install a component from a bundle.
 */
typedef struct {
    MfInstallMode mode;
    gboolean skip;
    const char *subdir;      /* of the store, where the component goes */
    const char *base_subdir; /* of the store, where the base is */
    char *base_component_id; /* NULL for a core */
    char *base_version;      /* NULL to use what is current */
    guint64 base_revision;
} MfdInstallPlan;

/**
 * mfd_install_plan_clear:
 */
static void
mfd_install_plan_clear(MfdInstallPlan *iplan)
{
    g_free(iplan->base_component_id);
    g_free(iplan->base_version);
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
            *ret_kind = mf_manifest_get_kind(manifest);
            iplan->base_subdir = mf_store_subdir_for_kind(*ret_kind, mf_manifest_is_sdk(manifest));
            iplan->base_component_id = g_strdup(component_id);
            iplan->base_version = current ? NULL : g_strdup(version);
            iplan->base_revision = current ? 0 : revision;
            return TRUE;
        }
    }

    if (installed == NULL)
        return FALSE;
    current = mf_component_is_current(installed);
    *ret_kind = mf_component_get_kind(installed);
    iplan->base_subdir = mf_store_subdir_for_kind(*ret_kind, mf_component_is_sdk(installed));
    iplan->base_component_id = g_strdup(mf_component_get_id(installed));
    iplan->base_version = current ? NULL : g_strdup(mf_component_get_version(installed));
    iplan->base_revision = current ? 0 : mf_component_get_revision(installed);
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
 * mfd_manager_plan_image_install:
 *
 * Plan how we install an image of the bundle: whether the component is
 * installed already, its install mode, and what it builds on. Everything
 * that refuses the installation of the image is checked here, before
 * anything is written.
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
    g_autoptr(MfComponent) existing = NULL;

    if (!g_str_equal(mf_manifest_get_architecture(manifest), mf_get_host_architecture())) {
        mfd_set_wrong_architecture_error(error,
                                         ret_detail,
                                         component_id,
                                         mf_manifest_get_architecture(manifest));
        return FALSE;
    }

    iplan->mode = mfd_bundle_is_dependency_of_app(bundle, manifest) ? MF_INSTALL_MODE_AUTO
                                                                    : MF_INSTALL_MODE_MANUAL;
    iplan->subdir = mf_store_subdir_for_kind(kind, sdk);

    existing = mf_store_get_component(manager->store, component_id, version, NULL);
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
        if (iplan->mode == MF_INSTALL_MODE_AUTO) {
            /* already installed (how it got there doesn't matter) */
            iplan->skip = TRUE;
            return TRUE;
        }
        if (mf_component_get_install_mode(existing) != MF_INSTALL_MODE_AUTO) {
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
        g_free(iplan->base_component_id);
        g_free(iplan->base_version);
        iplan->base_component_id = g_strdup(base_id);
        iplan->base_version = g_strdup(mf_manifest_get_base_version(manifest));
        iplan->base_revision = mf_manifest_get_base_revision(manifest);
    }

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
 * refused before anything is written.
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
    for (guint i = 0; i < images->len; i++) {
        MfManifest *manifest = mf_bundle_image_get_manifest(g_ptr_array_index(images, i));

        if (!mf_store_has_image(manager->store, mf_manifest_get_verity_hash(manifest)))
            tracker.total++;
    }
    for (guint i = 0; i < images->len; i++) {
        MfBundleImage *image = g_ptr_array_index(images, i);
        MfManifest *manifest = mf_bundle_image_get_manifest(image);
        const char *verity_hash = mf_manifest_get_verity_hash(manifest);
        const char *component_id = mf_manifest_get_component_id(manifest);
        const char *version = mf_manifest_get_version(manifest);
        guint64 image_size = mf_manifest_get_image_size(manifest);

        if (mf_store_has_image(manager->store, verity_hash)) {
            g_info("Image %s of %s %s is in the store already", verity_hash, component_id, version);
            continue;
        }

        g_info("Adding image %s of %s %s", verity_hash, component_id, version);
        mfd_progress_begin_item(&tracker, MF_ACTION_INSTALL, component_id, version, image_size);
        if (!mfd_store_writer_add_image(manager->writer,
                                        bundle,
                                        image,
                                        mfd_on_copy_progress,
                                        &tracker,
                                        error))
            return NULL;
        mfd_progress_end_item(&tracker);
        bytes_added += image_size;
    }

    for (guint i = 0; i < images->len; i++) {
        MfBundleImage *image = g_ptr_array_index(images, i);
        MfManifest *manifest = mf_bundle_image_get_manifest(image);
        const char *component_id = mf_manifest_get_component_id(manifest);
        const char *version = mf_manifest_get_version(manifest);
        MfdInstallPlan *plan = &g_array_index(plans, MfdInstallPlan, i);
        g_autoptr(GError) local_error = NULL;

        if (plan->skip)
            continue;

        if (!mfd_store_writer_install_component(manager->writer,
                                                plan->subdir,
                                                component_id,
                                                version,
                                                mf_manifest_get_revision(manifest),
                                                mf_manifest_get_verity_hash(manifest),
                                                plan->base_subdir,
                                                plan->base_component_id,
                                                plan->base_version,
                                                plan->base_revision,
                                                mf_manifest_get_data(manifest),
                                                mf_bundle_image_get_metainfo(image),
                                                plan->mode,
                                                &local_error)) {
            if (g_error_matches(local_error, MF_ERROR, MF_ERROR_ALREADY_INSTALLED))
                mfd_set_already_installed_error(error, ret_detail, component_id, version);
            else
                g_propagate_error(error, g_steal_pointer(&local_error));
            return NULL;
        }
    }

    /* emit the components of the bundle that are installed now as results */
    result = mf_result_new();
    for (guint i = 0; i < images->len; i++) {
        MfManifest *manifest = mf_bundle_image_get_manifest(g_ptr_array_index(images, i));
        g_autoptr(MfComponent) component = mf_store_get_component(manager->store,
                                                                  mf_manifest_get_component_id(manifest),
                                                                  mf_manifest_get_version(manifest),
                                                                  NULL);

        if (component == NULL ||
            g_strcmp0(mf_component_get_verity_hash(component), mf_manifest_get_verity_hash(manifest)) != 0)
            continue;
        mf_result_add_installed(result, component);
    }
    mf_result_set_bytes_added(result, bytes_added);

    return g_steal_pointer(&result);
}

/**
 * mfd_removal_plan_free:
 */
void
mfd_removal_plan_free(MfdRemovalPlan *rmplan)
{
    if (rmplan == NULL)
        return;

    g_ptr_array_unref(rmplan->components);
    g_ptr_array_unref(rmplan->dirs);
    g_ptr_array_unref(rmplan->images);
    g_free(rmplan);
}

/**
 * mfd_removal_plan_add_version_dir:
 * @rmplan: the #MfdRemovalPlan
 * @rm_pending: the directories that go, which the directory is added to
 * @component: the component the directory describes
 *
 * Add a version directory to what a removal removes.
 */
static void
mfd_removal_plan_add_version_dir(MfdRemovalPlan *rmplan,
                                 GHashTable *rm_pending,
                                 MfComponent *component,
                                 const char *version_dir)
{
    char *dir = g_strdup(version_dir);

    g_ptr_array_add(rmplan->components, g_object_ref(component));
    g_ptr_array_add(rmplan->dirs, dir);
    g_hash_table_add(rm_pending, dir);
}

/**
 * mfd_manager_find_dependent_dirs_for:
 * @components: (element-type MfComponent): what is installed
 * @dirs: the version directory of each of @components
 * @rm_pending: version directories whose links do not count
 *
 * Find what the `base` links of the components that stay refer to. A link
 * to `current` refers to the version `current` names, so nothing gets
 * downgraded under it. A base that can not be read is not protected.
 *
 * Returns: a table of every version directory a link names, mapped to the component-ID that links to it
 */
static GHashTable *
mfd_manager_find_dependent_dirs_for(MfdManager *manager,
                                    GPtrArray *components,
                                    GPtrArray *dirs,
                                    GHashTable *rm_pending)
{
    const char *root = mf_store_get_path(manager->store);
    GHashTable *used = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    for (guint i = 0; i < components->len; i++) {
        MfComponent *component = g_ptr_array_index(components, i);
        g_auto(GStrv) parts = NULL;

        if (g_hash_table_contains(rm_pending, g_ptr_array_index(dirs, i)))
            continue;
        parts = mf_store_read_base_link_at(root, component, NULL);
        if (parts == NULL)
            continue;
        /* a link to `current` keeps the version it names, to prevent accidental downgrades */
        if (g_str_equal(parts[2], MF_STORE_CURRENT_NAME)) {
            g_autofree char *current_path = g_build_filename(root,
                                                             parts[0],
                                                             parts[1],
                                                             MF_STORE_CURRENT_NAME,
                                                             NULL);
            char *target = g_file_read_link(current_path, NULL);

            if (target == NULL)
                continue;
            g_free(parts[2]);
            parts[2] = target;
        }
        g_hash_table_insert(used,
                            g_build_filename(root, parts[0], parts[1], parts[2], NULL),
                            (gpointer) mf_component_get_id(component));
    }

    return used;
}

/**
 * mfd_find_used_images:
 * @components: (element-type MfComponent): what is installed
 * @dirs: the version directory of each of @components
 * @rm_pending: version directories whose links do not count
 *
 * Returns: a set of the root hash of every image that is (or will be) still in use
 */
static GHashTable *
mfd_find_used_images(GPtrArray *components, GPtrArray *dirs, GHashTable *rm_pending)
{
    GHashTable *used = g_hash_table_new(g_str_hash, g_str_equal);

    for (guint i = 0; i < components->len; i++) {
        MfComponent *component = g_ptr_array_index(components, i);

        if (!g_hash_table_contains(rm_pending, g_ptr_array_index(dirs, i)))
            g_hash_table_add(used, (gpointer) mf_component_get_verity_hash(component));
    }

    return used;
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
    g_autoptr(MfdRemovalPlan) rmplan = g_new0(MfdRemovalPlan, 1);
    g_autoptr(GPtrArray) components = NULL;
    g_autoptr(GPtrArray) rm_dirs = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GHashTable) rm_pending = g_hash_table_new(g_str_hash, g_str_equal);
    g_autoptr(GHashTable) used = NULL;
    g_autoptr(GHashTable) images_used = NULL;
    g_autoptr(GDir) images = NULL;
    g_autofree char *images_dir = NULL;
    const char *name;

    g_return_val_if_fail(MFD_IS_MANAGER(manager), NULL);

    rmplan->components = g_ptr_array_new_with_free_func(g_object_unref);
    rmplan->dirs = g_ptr_array_new_with_free_func(g_free);
    rmplan->images = g_ptr_array_new_with_free_func(g_free);

    /* every manifest is read once here */
    components = mf_store_list_components(manager->store, NULL, MF_COMPONENT_KIND_UNKNOWN, error);
    if (components == NULL)
        return NULL;
    for (guint i = 0; i < components->len; i++)
        g_ptr_array_add(rm_dirs,
                        mf_store_get_component_dir(manager->store, g_ptr_array_index(components, i)));

    if (component_id != NULL) {
        for (guint i = 0; i < components->len; i++) {
            MfComponent *component = g_ptr_array_index(components, i);

            if (g_str_equal(mf_component_get_id(component), component_id) &&
                (version == NULL || g_str_equal(mf_component_get_version(component), version)))
                mfd_removal_plan_add_version_dir(rmplan,
                                                 rm_pending,
                                                 component,
                                                 g_ptr_array_index(rm_dirs, i));
        }
        if (rmplan->dirs->len == 0) {
            if (ret_detail != NULL) {
                mfd_error_detail_clear(ret_detail);
                ret_detail->component_id = g_strdup(component_id);
            }
            g_set_error(error,
                        MF_ERROR,
                        MF_ERROR_NOT_FOUND,
                        "%s%s%s is not installed",
                        component_id,
                        version != NULL ? "/" : "",
                        version != NULL ? version : "");
            return NULL;
        }

        /* nothing that stays may link to stuff that is going to be removed */
        used = mfd_manager_find_dependent_dirs_for(manager, components, rm_dirs, rm_pending);
        for (guint i = 0; i < rmplan->dirs->len; i++) {
            const char *used_by = g_hash_table_lookup(used, g_ptr_array_index(rmplan->dirs, i));

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
    } else {
        used = mfd_manager_find_dependent_dirs_for(manager, components, rm_dirs, rm_pending);
    }

    /* check for any cruft that we can drop now that nothing would depend on it anymore */
    for (guint i = 0; i < components->len; i++) {
        MfComponent *component = g_ptr_array_index(components, i);
        const char *version_dir = g_ptr_array_index(rm_dirs, i);

        if (mf_component_get_install_mode(component) != MF_INSTALL_MODE_AUTO ||
            g_hash_table_contains(rm_pending, version_dir) || g_hash_table_contains(used, version_dir))
            continue;
        g_info("%s is not needed anymore", version_dir);
        mfd_removal_plan_add_version_dir(rmplan, rm_pending, component, version_dir);
    }

    /* the images nothing links to, once the components are gone */
    images_used = mfd_find_used_images(components, rm_dirs, rm_pending);
    images_dir = g_build_filename(mf_store_get_path(manager->store), MF_STORE_IMAGES_SUBDIR, NULL);
    images = g_dir_open(images_dir, 0, error);
    if (images == NULL)
        return NULL;
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

    return g_steal_pointer(&rmplan);
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
 * Any cruft that interrupted operations left behind is deleted first, then the
 * component directories, then the images. The images of a component are
 * unlinked while they may be in use; their contents exist for as long as
 * something has them mounted.
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

    tracker.total = rmplan->dirs->len + rmplan->images->len;
    if (!mfd_store_writer_remove_cruft(manager->writer, error))
        return NULL;

    result = mf_result_new();
    for (guint i = 0; i < rmplan->dirs->len; i++) {
        MfComponent *component = g_ptr_array_index(rmplan->components, i);

        mfd_progress_begin_item(&tracker,
                                MF_ACTION_REMOVE,
                                mf_component_get_id(component),
                                mf_component_get_version(component),
                                0);
        if (!mfd_store_writer_remove_version_dir(manager->writer, g_ptr_array_index(rmplan->dirs, i), error))
            return NULL;
        mfd_progress_end_item(&tracker);
        mf_result_add_removed(result, component);
    }

    for (guint i = 0; i < rmplan->images->len; i++) {
        const char *hash = g_ptr_array_index(rmplan->images, i);

        mfd_progress_begin_item(&tracker, MF_ACTION_CLEANUP, hash, NULL, 0);
        if (!mfd_store_writer_remove_image(manager->writer, hash, error))
            return NULL;
        mfd_progress_end_item(&tracker);
        mf_result_add_removed_image(result, hash);
    }
    mf_result_set_bytes_freed(result, rmplan->image_bytes);

    return g_steal_pointer(&result);
}
