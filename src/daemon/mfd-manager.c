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
    detail->kind = MF_COMPONENT_KIND_UNKNOWN;
    detail->sdk = FALSE;
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
 * mfd_progress_split_last_item:
 * @n_items: the number of items to split the last item into
 *
 * Split the last item of the operation into @n_items items, which together
 * take the share of the progress that the last item had. We only know how
 * many items there are once the last item begins: removing what an
 * installation supersedes is one item, which consists of one item for every
 * component it removes. With no items, the last item is done right away.
 */
static void
mfd_progress_split_last_item(MfdProgressTracker *tracker, guint n_items)
{
    if (n_items == 0) {
        tracker->done = tracker->total;
        mfd_progress_report(tracker);
        return;
    }
    tracker->total *= n_items;
    tracker->done *= n_items;
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
    gboolean skip;      /* installed already, or not needed: there is nothing to do */
    gboolean add_image; /* the image has to be added to the store */
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
 * mfd_install_plan_compare_layer:
 *
 * Order plans by the layer of their component, from the bottom to the top.
 * A component only builds on a kind that is declared before its own kind,
 * or on its own kind if it is an SDK, see mf_component_kind_can_build_on().
 * In this order, every component therefore comes after the components it
 * builds on.
 */
static gint
mfd_install_plan_compare_layer(gconstpointer a, gconstpointer b)
{
    MfManifest *manifest_a = mf_bundle_image_get_manifest(((const MfdInstallPlan *) a)->image);
    MfManifest *manifest_b = mf_bundle_image_get_manifest(((const MfdInstallPlan *) b)->image);
    MfComponentKind kind_a = mf_manifest_get_kind(manifest_a);
    MfComponentKind kind_b = mf_manifest_get_kind(manifest_b);

    if (kind_a != kind_b)
        return kind_a < kind_b ? -1 : 1;
    /* an SDK builds on a component of its own kind */
    return (int) mf_manifest_is_sdk(manifest_a) - (int) mf_manifest_is_sdk(manifest_b);
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
 * mfd_set_in_use_error:
 * @used_by: the ID of a kept component that builds on it
 */
static void
mfd_set_in_use_error(GError **error,
                     MfdErrorDetail *ret_detail,
                     const char *component_id,
                     const char *used_by)
{
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
}

/**
 * mfd_set_missing_base_error:
 * @manifest: the manifest of the component whose base is missing
 * @base_version: the version of the base, with its revision if only that exact revision will do
 */
static void
mfd_set_missing_base_error(GError **error,
                           MfdErrorDetail *ret_detail,
                           MfManifest *manifest,
                           const char *base_version)
{
    const char *component_id = mf_manifest_get_component_id(manifest);
    const char *base_id = mf_manifest_get_base_id(manifest);
    const char *base_identity = mf_manifest_get_base_identity(manifest);

    if (ret_detail != NULL) {
        mfd_error_detail_clear(ret_detail);
        ret_detail->component_id = g_strdup(component_id);
        ret_detail->base_id = g_strdup(base_id);
        ret_detail->base_version = g_strdup(base_version);
        ret_detail->identity = g_strdup(base_identity);
    }
    g_set_error(error,
                MF_ERROR,
                MF_ERROR_MISSING_BASE,
                "%s needs base %s %s (identity %s), which is not installed",
                component_id,
                base_id,
                base_version,
                base_identity);
}

/**
 * mfd_set_id_conflict_error:
 * @installed: the installed version that has the ID, but is of another kind
 */
static void
mfd_set_id_conflict_error(GError **error, MfdErrorDetail *ret_detail, MfComponent *installed)
{
    const char *component_id = mf_component_get_id(installed);
    MfComponentKind kind = mf_component_get_kind(installed);
    gboolean sdk = mf_component_is_sdk(installed);

    if (ret_detail != NULL) {
        mfd_error_detail_clear(ret_detail);
        ret_detail->component_id = g_strdup(component_id);
        ret_detail->kind = kind;
        ret_detail->sdk = sdk;
    }
    g_set_error(error,
                MF_ERROR,
                MF_ERROR_ID_CONFLICT,
                "%s is installed already as another kind of component (%s%s)",
                component_id,
                mf_component_kind_to_string(kind),
                sdk ? " SDK" : "");
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
 * mfd_check_if_bundle_image_wins:
 * @manifest: the manifest of an image of the bundle that provides an identity
 * @installed: (nullable): what provides the identity in the store
 *
 * Check whether we use the image of the bundle that provides an identity, rather
 * than what provides the identity in the store. The image of the bundle wins if the
 * store has nothing that provides the identity, if that is another component, or if
 * it is an older version of the same component.
 *
 * Returns: %TRUE if the image of the bundle wins
 */
static gboolean
mfd_check_if_bundle_image_wins(MfManifest *manifest, MfComponent *installed)
{
    return installed == NULL ||
           g_strcmp0(mf_component_get_id(installed), mf_manifest_get_component_id(manifest)) != 0 ||
           mf_version_compare_with_rev(mf_component_get_version(installed),
                                       mf_component_get_revision(installed),
                                       mf_manifest_get_version(manifest),
                                       mf_manifest_get_revision(manifest)) < 0;
}

/**
 * mfd_manager_resolve_dependency:
 * @iplan: the plan whose base to set
 * @ret_kind: (out): the kind of the component that provides the identity
 * @ret_sdk: (out): whether the component that provides the identity is an SDK
 *
 * Find the component that provides the identity a component needs as its
 * base, in the bundle or in the store, and make it the base of @iplan.
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
                               MfComponentKind *ret_kind,
                               gboolean *ret_sdk)
{
    MfBundleImage *image = mf_bundle_find_image_for_identity(bundle, identity);
    g_autoptr(MfComponent) installed = mf_store_find_identity(manager->store, identity, NULL);
    gboolean current;

    if (image != NULL && mfd_check_if_bundle_image_wins(mf_bundle_image_get_manifest(image), installed)) {
        MfManifest *manifest = mf_bundle_image_get_manifest(image);
        const char *component_id = mf_manifest_get_component_id(manifest);
        const char *version = mf_manifest_get_version(manifest);
        guint64 revision = mf_manifest_get_revision(manifest);
        /* the bundle's image becomes current unless a newer version is already installed */
        g_autoptr(MfComponent) newest = mf_store_get_component(manager->store, component_id, NULL, NULL);
        g_autofree char *version_name = NULL;

        current = newest == NULL || mf_version_compare_with_rev(version,
                                                                revision,
                                                                mf_component_get_version(newest),
                                                                mf_component_get_revision(newest)) >= 0;
        version_name = current ? NULL : mf_build_full_version(version, revision);
        *ret_kind = mf_manifest_get_kind(manifest);
        *ret_sdk = mf_manifest_is_sdk(manifest);
        iplan->base = mf_base_link_new(mf_store_subdir_for_kind(*ret_kind, *ret_sdk),
                                       component_id,
                                       version_name);
        return TRUE;
    }

    if (installed == NULL)
        return FALSE;
    *ret_kind = mf_component_get_kind(installed);
    *ret_sdk = mf_component_is_sdk(installed);
    iplan->base = mf_base_link_new(
        mf_store_subdir_for_kind(*ret_kind, *ret_sdk),
        mf_component_get_id(installed),
        mf_component_is_current(installed) ? NULL : mf_component_get_full_version(installed));
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
 * mfd_manager_find_other_kind:
 *
 * An ID names exactly one kind of component.
 * This method searches the installed components for ones that
 * have the same ID as @manifest, but have a different kind.
 *
 * Returns: (transfer full) (nullable): an installed version with the same ID as @manifest, but of a different kind
 */
static MfComponent *
mfd_manager_find_other_kind(MfdManager *manager, MfManifest *manifest)
{
    g_autoptr(GPtrArray) versions = NULL;

    versions = mf_store_list_components(manager->store,
                                        mf_manifest_get_component_id(manifest),
                                        MF_COMPONENT_KIND_UNKNOWN,
                                        NULL);
    for (guint i = 0; versions != NULL && i < versions->len; i++) {
        MfComponent *component = g_ptr_array_index(versions, i);

        if (mf_component_get_kind(component) != mf_manifest_get_kind(manifest) ||
            mf_component_is_sdk(component) != mf_manifest_is_sdk(manifest))
            return g_object_ref(component);
    }
    return NULL;
}

/**
 * mfd_bundle_find_exact_base:
 *
 * Returns: (transfer none) (nullable): the manifest of the image in the
 *   bundle that has the component, version and revision @manifest names as its base
 */
static MfManifest *
mfd_bundle_find_exact_base(MfBundle *bundle, MfManifest *manifest)
{
    GPtrArray *images = mf_bundle_get_images(bundle);

    for (guint i = 0; i < images->len; i++) {
        MfManifest *base = mf_bundle_image_get_manifest(g_ptr_array_index(images, i));

        if (g_str_equal(mf_manifest_get_component_id(base), mf_manifest_get_base_id(manifest)) &&
            g_str_equal(mf_manifest_get_version(base), mf_manifest_get_base_version(manifest)) &&
            mf_manifest_get_revision(base) == mf_manifest_get_base_revision(manifest))
            return base;
    }

    return NULL;
}

/**
 * mfd_manager_resolve_exact_base:
 * @manifest: the manifest of the component whose base to find
 * @iplan: the plan whose base to set
 * @ret_kind: (out): the kind of the base
 * @ret_sdk: (out): whether the base is an SDK
 *
 * Find the exact version of the base a component was built against, in the
 * bundle or in the store, and make it the base of @iplan. Another version
 * that provides the same identity will not do.
 *
 * Returns: %TRUE if that version is there, and provides the identity the
 *   component needs.
 */
static gboolean
mfd_manager_resolve_exact_base(MfdManager *manager,
                               MfBundle *bundle,
                               MfManifest *manifest,
                               MfdInstallPlan *iplan,
                               MfComponentKind *ret_kind,
                               gboolean *ret_sdk)
{
    const char *base_id = mf_manifest_get_base_id(manifest);
    const char *base_version = mf_manifest_get_base_version(manifest);
    guint64 base_revision = mf_manifest_get_base_revision(manifest);
    MfManifest *in_bundle = mfd_bundle_find_exact_base(bundle, manifest);
    g_autoptr(MfComponent) installed = NULL;
    g_autofree char *version_name = NULL;
    const char *identity;
    MfComponentKind kind;
    gboolean sdk;

    if (in_bundle != NULL) {
        identity = mf_manifest_get_identity(in_bundle);
        kind = mf_manifest_get_kind(in_bundle);
        sdk = mf_manifest_is_sdk(in_bundle);
    } else {
        installed = mfd_manager_get_installed(manager, base_id, base_version, base_revision);
        if (installed == NULL)
            return FALSE;
        identity = mf_component_get_identity(installed);
        kind = mf_component_get_kind(installed);
        sdk = mf_component_is_sdk(installed);
    }
    if (g_strcmp0(identity, mf_manifest_get_base_identity(manifest)) != 0)
        return FALSE;

    version_name = mf_build_full_version(base_version, base_revision);
    iplan->base = mf_base_link_new(mf_store_subdir_for_kind(kind, sdk), base_id, version_name);
    *ret_kind = kind;
    *ret_sdk = sdk;
    return TRUE;
}

/**
 * mfd_manager_plan_image_install:
 *
 * Plan how we install an image of the bundle: whether we add the image to
 * the store, whether the component is installed already or not needed at
 * all, its install mode, and what it builds on. Everything that refuses the
 * installation of the image is checked here, before anything is written.
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
    g_autoptr(MfComponent) other_kind = NULL;
    g_autoptr(MfComponent) existing = NULL;

    if (!g_str_equal(mf_manifest_get_architecture(manifest), mf_get_host_architecture())) {
        mfd_set_wrong_architecture_error(error,
                                         ret_detail,
                                         component_id,
                                         mf_manifest_get_architecture(manifest));
        return FALSE;
    }
    other_kind = mfd_manager_find_other_kind(manager, manifest);
    if (other_kind != NULL) {
        mfd_set_id_conflict_error(error, ret_detail, other_kind);
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

    /* a kit that came along with an application is not needed if the application can run on
     * a newer version that we already have in the store */
    if (dependency) {
        g_autoptr(MfComponent)
            provider = mf_store_find_identity(manager->store, mf_manifest_get_identity(manifest), NULL);

        if (!mfd_check_if_bundle_image_wins(manifest, provider)) {
            g_info("%s %s is not needed, %s is installed",
                   component_id,
                   version,
                   mf_component_get_full_version(provider));
            iplan->skip = TRUE;
            iplan->add_image = FALSE;
            return TRUE;
        }
    }

    if (base_id != NULL) {
        const char *base_version = mf_manifest_get_base_version(manifest);
        MfComponentKind base_kind;
        gboolean base_sdk;

        /* a platform and an SDK need the exact version they were built against,
         * everything else builds on whatever provides the identity of its base */
        if (sdk || kind == MF_COMPONENT_KIND_PLATFORM) {
            if (!mfd_manager_resolve_exact_base(manager, bundle, manifest, iplan, &base_kind, &base_sdk)) {
                g_autofree char *base_version_name = mf_build_full_version(
                    base_version,
                    mf_manifest_get_base_revision(manifest));

                mfd_set_missing_base_error(error, ret_detail, manifest, base_version_name);
                return FALSE;
            }
        } else if (!mfd_manager_resolve_dependency(manager,
                                                   bundle,
                                                   mf_manifest_get_base_identity(manifest),
                                                   iplan,
                                                   &base_kind,
                                                   &base_sdk)) {
            mfd_set_missing_base_error(error, ret_detail, manifest, base_version);
            return FALSE;
        }

        /* a component can only build on the kinds of layer that are allowed below it */
        if (!mf_component_kind_can_build_on(kind, sdk, base_kind, base_sdk)) {
            g_set_error(error,
                        MF_ERROR,
                        MF_ERROR_INVALID_BUNDLE,
                        "%s: %s%s %s can not build on %s%s %s",
                        mf_bundle_image_get_manifest_name(image),
                        mf_component_kind_to_string(kind),
                        sdk ? " SDK" : "",
                        component_id,
                        mf_component_kind_to_string(base_kind),
                        base_sdk ? " SDK" : "",
                        iplan->base->component_id);
            return FALSE;
        }
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

    g_hash_table_unref(rmplan->cpts_set);
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
    /* the set holds the components of the items, which own them */
    rmplan->cpts_set = g_hash_table_new(g_direct_hash, g_direct_equal);
    rmplan->images = g_ptr_array_new_with_free_func(g_free);
    return rmplan;
}

/**
 * mfd_removal_plan_add_item:
 * @rmplan: the #MfdRemovalPlan
 * @component: the version that will be removed
 * @action: why it will be removed, see #MfdRemovalItem
 *
 * Add a component to a removal plan.
 */
static void
mfd_removal_plan_add_item(MfdRemovalPlan *rmplan, MfComponent *component, MfAction action)
{
    MfdRemovalItem *item = g_new0(MfdRemovalItem, 1);

    item->component = g_object_ref(component);
    item->action = action;
    g_ptr_array_add(rmplan->items, item);
    g_hash_table_add(rmplan->cpts_set, item->component);
}

/**
 * mfd_manager_find_used:
 * @installed: (element-type MfComponent): every installed version
 * @rm_pending: (element-type MfComponent): versions that should be ignored because they will be removed
 *
 * Find the versions that the `base` links of the other installed versions
 * point to. A link to `current` keeps the version that `current` points to
 * now: if we removed it, `current` would move to an older version, and the
 * versions that link to it would build on that one. We do not keep a base
 * whose link we can not read, or that is not installed.
 *
 * Returns: every version a link points to, mapped to a version that links to it
 */
static GHashTable *
mfd_manager_find_used(MfdManager *manager, GPtrArray *installed, GHashTable *rm_pending)
{
    GHashTable *used = g_hash_table_new_full(g_direct_hash, g_direct_equal, g_object_unref, NULL);

    for (guint i = 0; i < installed->len; i++) {
        MfComponent *component = g_ptr_array_index(installed, i);
        MfComponent *base;

        if (g_hash_table_contains(rm_pending, component))
            continue;
        base = mf_store_get_base(manager->store, component, NULL);
        if (base != NULL)
            g_hash_table_insert(used, base, component);
    }

    return used;
}

/**
 * mfd_find_used_images:
 * @installed: (element-type MfComponent): every installed version
 * @rm_pending: (element-type MfComponent): versions pending deletion
 *
 * Returns: a set of the root hash of every image that is (or will be) still in use
 */
static GHashTable *
mfd_find_used_images(GPtrArray *installed, GHashTable *rm_pending)
{
    GHashTable *used = g_hash_table_new(g_str_hash, g_str_equal);

    for (guint i = 0; i < installed->len; i++) {
        MfComponent *component = g_ptr_array_index(installed, i);

        if (!g_hash_table_contains(rm_pending, component))
            g_hash_table_add(used, (gpointer) mf_component_get_verity_hash(component));
    }

    return used;
}

/**
 * mfd_find_newest_apps:
 * @installed: (element-type MfComponent): every installed version
 * @rm_pending: (element-type MfComponent): versions that will be removed
 *
 * Find the newest version of every application and plugin that is not
 * planned for removal. That version is kept whatever its install mode is.
 *
 * Returns: a set of those versions
 */
static GHashTable *
mfd_find_newest_apps(GPtrArray *installed, GHashTable *rm_pending)
{
    /* the component-ID mapped to its newest version */
    g_autoptr(GHashTable) newest = g_hash_table_new(g_str_hash, g_str_equal);
    GHashTable *kept = g_hash_table_new(g_direct_hash, g_direct_equal);
    GHashTableIter iter;
    gpointer value;

    for (guint i = 0; i < installed->len; i++) {
        MfComponent *component = g_ptr_array_index(installed, i);
        MfComponent *other;

        if (!mf_component_kind_is_always_auto(mf_component_get_kind(component),
                                              mf_component_is_sdk(component)) ||
            g_hash_table_contains(rm_pending, component))
            continue;
        other = g_hash_table_lookup(newest, mf_component_get_id(component));
        if (other == NULL || mf_version_compare_with_rev(mf_component_get_version(component),
                                                         mf_component_get_revision(component),
                                                         mf_component_get_version(other),
                                                         mf_component_get_revision(other)) > 0)
            g_hash_table_insert(newest, (gpointer) mf_component_get_id(component), component);
    }

    g_hash_table_iter_init(&iter, newest);
    while (g_hash_table_iter_next(&iter, NULL, &value))
        g_hash_table_add(kept, value);

    return kept;
}

/**
 * mfd_manager_complete_removal_plan:
 * @rmplan: the removal plan, which may already list versions planned for removal
 * @installed: (element-type MfComponent): every installed version
 * @requested: (element-type MfComponent): the versions the removal was requested for
 *
 * Work out everything a removal will delete, in the order it has to go.
 *
 * A version is planned for removal if it was requested, or if it is an application
 * or plugin, marked as auto, and not the latest version. This can happen only once
 * no kept version links to it anymore. Removing a version can leave what it links to
 * unused, e.g. the kit of an application, so this is repeated until no more
 * versions are planned for removal. A version is thus always removed last, after
 * every version that links to it. A requested version that a kept version
 * still links to is not planned for removal.
 *
 * Then every image that no kept version uses is planned for removal, too.
 */
static gboolean
mfd_manager_complete_removal_plan(MfdManager *manager,
                                  MfdRemovalPlan *rmplan,
                                  GPtrArray *installed,
                                  GHashTable *requested,
                                  GError **error)
{
    g_autoptr(GHashTable) newest_apps = NULL;
    g_autoptr(GHashTable) images_used = NULL;
    g_autoptr(GPtrArray) images = NULL;
    gboolean added;

    /* the newest version of an application or plugin that was not requested is kept, even if it is auto */
    newest_apps = mfd_find_newest_apps(installed, requested);
    do {
        /* the versions that are linked to by versions not planned for removal yet */
        g_autoptr(GHashTable) used = mfd_manager_find_used(manager, installed, rmplan->cpts_set);

        added = FALSE;
        for (guint i = 0; i < installed->len; i++) {
            MfComponent *component = g_ptr_array_index(installed, i);

            if (g_hash_table_contains(rmplan->cpts_set, component) || g_hash_table_contains(used, component))
                continue;
            if (g_hash_table_contains(requested, component)) {
                mfd_removal_plan_add_item(rmplan, component, MF_ACTION_REMOVE);
            } else if (mf_component_get_install_mode(component) == MF_INSTALL_MODE_AUTO &&
                       !g_hash_table_contains(newest_apps, component)) {
                g_info("%s is not needed anymore", mf_component_get_dir(component));
                mfd_removal_plan_add_item(rmplan, component, MF_ACTION_CLEANUP);
            } else {
                /* kept: marked manual, or the newest version of an application or plugin */
                continue;
            }
            added = TRUE;
        }
        /* a version planned for removal in this round may have been the only one that linked to
         * another version, which can then be removed as well */
    } while (added);

    /* plan to remove the images that no kept version uses */
    images_used = mfd_find_used_images(installed, rmplan->cpts_set);
    images = mf_store_list_images(manager->store, error);
    if (images == NULL)
        return FALSE;
    for (guint i = 0; i < images->len; i++) {
        const char *hash = g_ptr_array_index(images, i);
        g_autofree char *path = NULL;
        struct stat st;

        if (g_hash_table_contains(images_used, hash))
            continue;
        path = mf_store_image_path_at(mf_store_get_path(manager->store), hash);
        if (stat(path, &st) == 0)
            rmplan->image_bytes += (guint64) st.st_size;
        g_ptr_array_add(rmplan->images, g_strdup(hash));
    }

    return TRUE;
}

/**
 * mfd_version_selects:
 * @version: a version, or a version with its revision if @full
 *
 * Check whether @version names @component, an installed version of a component.
 */
static gboolean
mfd_version_selects(MfComponent *component, const char *version, gboolean full)
{
    return g_str_equal(full ? mf_component_get_full_version(component) : mf_component_get_version(component),
                       version);
}

/**
 * mfd_manager_resolve_version:
 * @version: (nullable): a version, which selects all of its revisions, or
 *   `<version>-<rev>`, which selects one
 * @ret_full: (out): whether @version is a version with its revision
 *
 * Find out whether @version is a version or a version with its revision, by
 * comparing it with the installed versions of the component. A version can
 * end in something that looks like a revision, so we refuse a @version that
 * names one installed version as a version, and another one as a version
 * with its revision.
 */
static gboolean
mfd_manager_resolve_version(MfdManager *manager,
                            const char *component_id,
                            const char *version,
                            gboolean *ret_full,
                            GError **error)
{
    g_autoptr(GPtrArray) components = NULL;
    gboolean as_version = FALSE;
    gboolean as_full = FALSE;

    *ret_full = FALSE;
    if (version == NULL)
        return TRUE;
    components = mf_store_list_components(manager->store, component_id, MF_COMPONENT_KIND_UNKNOWN, error);
    if (components == NULL)
        return FALSE;
    for (guint i = 0; i < components->len; i++) {
        MfComponent *component = g_ptr_array_index(components, i);

        as_version |= mfd_version_selects(component, version, FALSE);
        as_full |= mfd_version_selects(component, version, TRUE);
    }
    if (as_version && as_full) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_FAILED,
                    "%s %s is ambiguous: it names a version, and also another version with its revision",
                    component_id,
                    version);
        return FALSE;
    }

    *ret_full = as_full;
    return TRUE;
}

/**
 * mfd_manager_plan_removal:
 * @manager: an #MfdManager
 * @component_id: (nullable): a component to remove, or %NULL to only clean up cruft
 * @version: (nullable): the version to remove, with all of its revisions,
 *   `<version>-<rev>` to  specify the rev too, or %NULL for all versions
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
    g_autoptr(GHashTable) requested = g_hash_table_new(g_direct_hash, g_direct_equal);
    gboolean full = FALSE;

    g_return_val_if_fail(MFD_IS_MANAGER(manager), NULL);

    installed = mf_store_list_components(manager->store, NULL, MF_COMPONENT_KIND_UNKNOWN, error);
    if (installed == NULL)
        return NULL;

    if (component_id != NULL) {
        if (!mfd_manager_resolve_version(manager, component_id, version, &full, error))
            return NULL;
        for (guint i = 0; i < installed->len; i++) {
            MfComponent *candidate = g_ptr_array_index(installed, i);

            if (g_str_equal(mf_component_get_id(candidate), component_id) &&
                (version == NULL || mfd_version_selects(candidate, version, full)))
                g_hash_table_add(requested, candidate);
        }
        if (g_hash_table_size(requested) == 0) {
            mfd_set_not_installed_error(error, ret_detail, component_id, version);
            return NULL;
        }
    }

    if (!mfd_manager_complete_removal_plan(manager, rmplan, installed, requested, error))
        return NULL;

    /* a requested version that is not planned for removal is still linked to by a kept version */
    for (guint i = 0; i < installed->len; i++) {
        MfComponent *candidate = g_ptr_array_index(installed, i);
        g_autoptr(GHashTable) used = NULL;

        if (!g_hash_table_contains(requested, candidate) ||
            g_hash_table_contains(rmplan->cpts_set, candidate))
            continue;
        used = mfd_manager_find_used(manager, installed, rmplan->cpts_set);
        mfd_set_in_use_error(error,
                             ret_detail,
                             component_id,
                             mf_component_get_id(g_hash_table_lookup(used, candidate)));
        return NULL;
    }

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
    g_autoptr(GHashTable) superseded = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_autoptr(GHashTable) used = NULL;
    gboolean kept;

    installed = mf_store_list_components(manager->store, NULL, MF_COMPONENT_KIND_UNKNOWN, error);
    if (installed == NULL)
        return NULL;

    for (guint i = 0; i < installed->len; i++) {
        MfComponent *component = g_ptr_array_index(installed, i);

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
                g_hash_table_add(superseded, component);
                break;
            }
        }
    }

    /* we keep a superseded version that a version we keep links to, and repeat that, as the
     * kept version may link to another superseded version in turn */
    do {
        GHashTableIter iter;
        gpointer component;

        g_clear_pointer(&used, g_hash_table_unref);
        used = mfd_manager_find_used(manager, installed, superseded);
        kept = FALSE;
        g_hash_table_iter_init(&iter, superseded);
        while (g_hash_table_iter_next(&iter, &component, NULL)) {
            MfComponent *used_by = g_hash_table_lookup(used, component);

            if (used_by == NULL)
                continue;
            g_info("%s is superseded, but %s still builds on it",
                   mf_component_get_dir(component),
                   mf_component_get_id(used_by));
            g_hash_table_iter_remove(&iter);
            kept = TRUE;
        }
    } while (kept);

    for (guint i = 0; i < installed->len; i++) {
        MfComponent *component = g_ptr_array_index(installed, i);

        if (g_hash_table_contains(superseded, component))
            mfd_removal_plan_add_item(rmplan, component, MF_ACTION_CLEANUP);
    }

    /* an installation that supersedes nothing removes nothing else either */
    if (rmplan->items->len > 0 &&
        !mfd_manager_complete_removal_plan(manager, rmplan, installed, superseded, error))
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
        if (!mfd_store_writer_remove_component(manager->writer, item->component, error))
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
 * mfd_manager_execute_install_plan:
 * @bundle: the bundle the component is in
 * @iplan: the plan of the component
 * @tracker: the progress of the installation
 * @ret_bytes_added: (out): the size of the image if we added it to the store, or 0
 * @ret_detail: (out caller-allocates) (optional): more about a failure
 * @error: a #GError
 *
 * Install one component of a bundle: copy its image into the store, unless
 * the store has it already, and then record the component.
 *
 * If recording the component fails, we remove the image again if we added it,
 * so a failed step leaves nothing behind. A failed copy leaves nothing behind
 * by itself, see mfd_store_writer_add_image().
 *
 * Returns: %TRUE if the component was installed
 */
static gboolean
mfd_manager_execute_install_plan(MfdManager *manager,
                                 MfBundle *bundle,
                                 MfdInstallPlan *iplan,
                                 MfdProgressTracker *tracker,
                                 guint64 *ret_bytes_added,
                                 MfdErrorDetail *ret_detail,
                                 GError **error)
{
    MfManifest *manifest = mf_bundle_image_get_manifest(iplan->image);
    const char *verity_hash = mf_manifest_get_verity_hash(manifest);
    const char *component_id = mf_manifest_get_component_id(manifest);
    const char *version = mf_manifest_get_version(manifest);
    guint64 image_size = mf_manifest_get_image_size(manifest);
    g_autoptr(GError) local_error = NULL;
    gboolean added = FALSE;

    *ret_bytes_added = 0;
    if (iplan->add_image)
        mfd_progress_begin_item(tracker, MF_ACTION_INSTALL, component_id, version, image_size);

    /* another component of the bundle may have had the same image */
    if (iplan->add_image && !mf_store_has_image(manager->store, verity_hash)) {
        g_info("Adding image %s of %s %s", verity_hash, component_id, version);
        if (!mfd_store_writer_add_image(manager->writer,
                                        bundle,
                                        iplan->image,
                                        mfd_on_copy_progress,
                                        tracker,
                                        error))
            return FALSE;
        added = TRUE;
    } else {
        g_info("Image %s of %s %s is in the store already", verity_hash, component_id, version);
    }

    if (!mfd_store_writer_install_component(manager->writer,
                                            manifest,
                                            iplan->base,
                                            mf_bundle_image_get_metainfo(iplan->image),
                                            iplan->mode,
                                            &local_error)) {
        g_autoptr(GError) remove_error = NULL;

        /* the same version was installed in the meantime, and it uses the image, so we keep it */
        if (g_error_matches(local_error, MF_ERROR, MF_ERROR_ALREADY_INSTALLED)) {
            mfd_set_already_installed_error(error, ret_detail, component_id, version);
            return FALSE;
        }
        if (added && !mfd_store_writer_remove_image(manager->writer, verity_hash, &remove_error))
            g_warning("Unable to remove image %s again, the next cleanup will remove it: %s",
                      verity_hash,
                      remove_error->message);
        g_propagate_error(error, g_steal_pointer(&local_error));
        return FALSE;
    }

    if (iplan->add_image)
        mfd_progress_end_item(tracker);
    if (added)
        *ret_bytes_added = image_size;
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
 * Install everything a bundle contains. We refuse everything that can be
 * refused before we write anything. Then we install the components one after
 * the other, every component after the components of the bundle it builds on,
 * see mfd_manager_execute_install_plan().
 *
 * If installing a component fails, we undo only that step and stop. The
 * components that were installed before stay installed, so their images do
 * not have to be copied (or downloaded) again. Once all components are
 * installed, we remove the older versions and revisions the installation
 * supersedes, see mfd_manager_plan_supersede().
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
    /* we install every component after the components it builds on, so if installing a
     * component fails, the components installed before it have what they build on */
    g_array_sort(plans, mfd_install_plan_compare_layer);

    /* every image we copy is one item of the progress, and removing what the installation
     * supersedes is one item more */
    tracker.total = 1;
    for (guint i = 0; i < plans->len; i++) {
        if (g_array_index(plans, MfdInstallPlan, i).add_image)
            tracker.total++;
    }
    for (guint i = 0; i < plans->len; i++) {
        MfdInstallPlan *plan = &g_array_index(plans, MfdInstallPlan, i);
        guint64 image_bytes;

        if (plan->skip)
            continue;
        if (!mfd_manager_execute_install_plan(manager,
                                              bundle,
                                              plan,
                                              &tracker,
                                              &image_bytes,
                                              ret_detail,
                                              error))
            return NULL;
        bytes_added += image_bytes;
    }

    /* we remove what the installation supersedes, but the installation stands even if that fails */
    result = mf_result_new();
    rmplan = mfd_manager_plan_supersede(manager, plans, &supersede_error);
    mfd_progress_split_last_item(&tracker, rmplan != NULL ? rmplan->items->len : 0);
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
 * @version: (nullable): the version to remove, with all of its revisions, `<version>-<rev>`
 *     to specify the revision too, or %NULL for all versions
 * @progress_func: (nullable): called whenever the operation progresses
 * @user_data: passed to @progress_func
 * @ret_detail: (out caller-allocates) (optional): more about a failure
 * @error: a #GError
 *
 * Remove a component, or one version of it, as long as no kept version of
 * another component builds on it.
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
 * Check whether marking @version of a component marks @component, see
 * mfd_version_selects(). Without a @version, we mark the current version.
 */
static gboolean
mfd_manager_mark_selects(MfComponent *component, const char *version, gboolean full)
{
    if (version == NULL)
        return mf_component_is_current(component);
    return mfd_version_selects(component, version, full);
}

/**
 * mfd_manager_mark:
 * @manager: an #MfdManager
 * @component_id: the component to mark
 * @version: (nullable): the version to mark, with all of its revisions,
 *   `<version>-<rev>` to specify the rev too, or %NULL for the current one
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
    gboolean full = FALSE;
    guint n_marked = 0;

    g_return_val_if_fail(MFD_IS_MANAGER(manager), NULL);
    g_return_val_if_fail(mode == MF_INSTALL_MODE_MANUAL || mode == MF_INSTALL_MODE_AUTO, NULL);

    if (!mf_store_load(manager->store, error))
        return NULL;
    if (!mfd_manager_resolve_version(manager, component_id, version, &full, error))
        return NULL;
    components = mf_store_list_components(manager->store, component_id, MF_COMPONENT_KIND_UNKNOWN, error);
    if (components == NULL)
        return NULL;

    for (guint i = 0; i < components->len; i++) {
        MfComponent *component = g_ptr_array_index(components, i);

        if (!mfd_manager_mark_selects(component, version, full))
            continue;
        if (!mfd_store_writer_set_mode(manager->writer, component, mode, error))
            return NULL;
        g_message("Marked %s as %s", mf_component_get_dir(component), mf_install_mode_to_string(mode));
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

        if (mfd_manager_mark_selects(component, version, full))
            g_ptr_array_add(marked, g_object_ref(component));
    }

    return g_steal_pointer(&marked);
}
