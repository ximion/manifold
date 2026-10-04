/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define _GNU_SOURCE

#include "config.h"
#include "mfr-composer.h"

#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <libglnx.h>

#include "mfr-capture.h"
#include "mfr-image-mounts.h"

/**
 * MfrComposer:
 *
 * Composes the mount trees applications are launched in: loads what is
 * installed, mounts the images, captures the graphics stack of the host
 * for a core, and assembles a tree from these, which is kept for as long
 * as the application still consists of what it was made from.
 *
 * This is on the hot path of every launch. A launch of an application whose
 * tree is cached reads the store and compares the modification times of the
 * host's library directories, and does nothing else.
 */
struct _MfrComposer {
    GObject parent_instance;

    GHashTable *trees;          /* component id -> MfrTree */
    GHashTable *captures;       /* root hash of a core -> MfrCapture */
    MfrImageMounts *img_mounts; /* the mounted layer images of every tree */
};

G_DEFINE_FINAL_TYPE(MfrComposer, mfr_composer, G_TYPE_OBJECT)

/**
 * mfr_composer_release_images:
 *
 * Let go of the images of the first @n_layers layers of an application.
 */
static void
mfr_composer_release_images(MfrComposer *composer, const MfrApp *app, MfrLayerKind n_layers)
{
    for (MfrLayerKind kind = 0; kind < n_layers; kind++) {
        if (app->layers[kind].verity_hash != NULL)
            mfr_image_mounts_release(composer->img_mounts, app->layers[kind].verity_hash);
    }
}

/**
 * mfr_composer_drop_tree:
 *
 * Take a tree down, and let go of its images. The tree must have been
 * removed from the table already.
 */
static void
mfr_composer_drop_tree(MfrComposer *composer, MfrTree *tree)
{
    mfr_tree_teardown(tree);
    mfr_composer_release_images(composer, mfr_tree_get_app(tree), MFR_LAYER_COUNT);
    g_object_unref(tree);
}

/**
 * mfr_composer_drop_all_trees:
 *
 * Take all trees down, and let go of their mounts.
 */
static void
mfr_composer_drop_all_trees(MfrComposer *composer)
{
    GHashTableIter iter;
    gpointer value;

    g_hash_table_iter_init(&iter, composer->trees);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        g_hash_table_iter_remove(&iter);
        mfr_composer_drop_tree(composer, value);
    }
}

/**
 * mfr_composer_init:
 */
static void
mfr_composer_init(MfrComposer *composer)
{
    composer->trees = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    composer->captures = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    composer->img_mounts = mfr_image_mounts_new();
}

/**
 * mfr_composer_dispose:
 */
static void
mfr_composer_dispose(GObject *object)
{
    MfrComposer *composer = MFR_COMPOSER(object);

    /* what was launched keeps its clone of a tree, but the trees themselves die with us */
    if (composer->trees != NULL)
        mfr_composer_drop_all_trees(composer);

    G_OBJECT_CLASS(mfr_composer_parent_class)->dispose(object);
}

/**
 * mfr_composer_finalize:
 */
static void
mfr_composer_finalize(GObject *object)
{
    MfrComposer *composer = MFR_COMPOSER(object);

    g_hash_table_unref(composer->trees);
    g_hash_table_unref(composer->captures);
    mfr_image_mounts_free(composer->img_mounts);

    G_OBJECT_CLASS(mfr_composer_parent_class)->finalize(object);
}

/**
 * mfr_composer_class_init:
 */
static void
mfr_composer_class_init(MfrComposerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->dispose = mfr_composer_dispose;
    object_class->finalize = mfr_composer_finalize;
}

/**
 * mfr_enter_mount_namespace:
 *
 * Move the daemon into a mount namespace of its own, in which the mounts of
 * the host remain visible, and set up where images and trees are mounted.
 * After this, nothing mounted by us is seen by the host.
 */
static gboolean
mfr_enter_mount_namespace(GError **error)
{
    if (unshare(CLONE_NEWNS) != 0)
        return glnx_throw_errno_prefix(error, "Unable to create a mount namespace");
    /* changes to mounts of the host reach us, but ours do not reach the host */
    if (mount(NULL, "/", NULL, MS_SLAVE | MS_REC, NULL) != 0)
        return glnx_throw_errno_prefix(error, "Unable to make mounts private");

    if (!glnx_shutil_mkdir_p_at(AT_FDCWD, MF_RUN_MOUNT_DIR, 0755, NULL, error))
        return FALSE;
    if (mount("tmpfs", MF_RUN_MOUNT_DIR, "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755") != 0)
        return glnx_throw_errno_prefix(error, "Unable to mount tmpfs at %s", MF_RUN_MOUNT_DIR);
    /* a tree binds the host root, which must not include our own mounts */
    if (mount(NULL, MF_RUN_MOUNT_DIR, NULL, MS_UNBINDABLE | MS_REC, NULL) != 0)
        return glnx_throw_errno_prefix(error, "Unable to make %s unbindable", MF_RUN_MOUNT_DIR);

    for (const char *const *dir = (const char *const[]){MFR_IMAGES_MOUNT_DIR,
                                                        MFR_TREES_MOUNT_DIR,
                                                        MFR_TREES_CACHE_DIR,
                                                        MFR_CAPTURE_DIR,
                                                        NULL};
         *dir != NULL;
         dir++) {
        if (!glnx_shutil_mkdir_p_at(AT_FDCWD, *dir, 0755, NULL, error))
            return FALSE;
    }

    return TRUE;
}

/**
 * mfr_composer_new:
 * @error: A #GError
 *
 * Create the composer. This moves the whole process into a mount namespace
 * of its own, so there can only be one.
 */
MfrComposer *
mfr_composer_new(GError **error)
{
    if (!mfr_enter_mount_namespace(error))
        return NULL;

    /* clean up captures in case anything has changed on our host, and trees nothing uses anymore */
    mfr_capture_check_all();
    mfr_tree_remove_unused();

    return g_object_new(MFR_TYPE_COMPOSER, NULL);
}

/**
 * mfr_composer_lookup_capture:
 *
 * Look up the capture we loaded already for the core of an application. We
 * only return it if the host has not changed since it was made.
 */
static MfrCapture *
mfr_composer_lookup_capture(MfrComposer *composer, const MfrApp *app)
{
    const char *core_hash = app->layers[MFR_LAYER_CORE].verity_hash;
    MfrCapture *capture = g_hash_table_lookup(composer->captures, core_hash);

    if (capture == NULL)
        return NULL;
    if (!mfr_capture_is_fresh(capture)) {
        g_hash_table_remove(composer->captures, core_hash);
        return NULL;
    }
    return capture;
}

/**
 * mfr_composer_load_capture:
 *
 * Load the capture for the core of an application, which is mounted at
 * @core_mount: the one in the cache, or a new one if there is none, or if
 * the host has changed.
 *
 * Returns: (transfer none): the capture, owned by the composer
 */
static MfrCapture *
mfr_composer_load_capture(MfrComposer *composer, const MfrApp *app, const char *core_mount, GError **error)
{
    const char *core_hash = app->layers[MFR_LAYER_CORE].verity_hash;
    MfrCapture *capture;

    capture = mfr_capture_ensure(core_hash, core_mount, app->tuple, error);
    if (capture == NULL)
        return NULL;
    g_hash_table_insert(composer->captures, g_strdup(core_hash), capture);

    return capture;
}

/**
 * mfr_composer_prepare:
 * @composer: The composer.
 * @component_id: identifier of the application to launch
 * @error: A #GError
 *
 * Prepare the tree to launch an application in. We reuse the cached tree if
 * the application still consists of what that tree was made from, and make a
 * new one otherwise.
 *
 * Returns: (transfer full): the tree
 */
MfrTree *
mfr_composer_prepare(MfrComposer *composer, const char *component_id, GError **error)
{
    g_autoptr(MfrApp) app = NULL;
    const char *image_paths[MFR_LAYER_COUNT] = {0};
    MfrCapture *capture;
    MfrTree *tree;
    MfrLayerKind acquired = 0;

    app = mfr_app_load(component_id, error);
    if (app == NULL)
        return NULL;

    tree = g_hash_table_lookup(composer->trees, component_id);
    capture = mfr_composer_lookup_capture(composer, app);
    if (tree != NULL && capture != NULL && mfr_tree_matches(tree, app, capture))
        return g_object_ref(tree);

    /* no tree can be reused as it is: mount the images, and capture the host
     * again unless the capture is still good */
    for (; acquired < MFR_LAYER_COUNT; acquired++) {
        if (app->layers[acquired].verity_hash == NULL)
            continue;
        image_paths[acquired] = mfr_image_mounts_acquire(composer->img_mounts, &app->layers[acquired], error);
        if (image_paths[acquired] == NULL) {
            mfr_composer_release_images(composer, app, acquired);
            return NULL;
        }
    }
    if (capture == NULL) {
        capture = mfr_composer_load_capture(composer, app, image_paths[MFR_LAYER_CORE], error);
        if (capture == NULL) {
            mfr_composer_release_images(composer, app, MFR_LAYER_COUNT);
            return NULL;
        }
    }

    /* the old tree is dropped only now, so that the images it shares with the new one stay mounted */
    if (tree != NULL) {
        g_info("Dropping tree %s of %s, the application or the host changed",
               mfr_tree_get_key(tree),
               component_id);
        g_hash_table_remove(composer->trees, component_id);
        mfr_composer_drop_tree(composer, tree);
    }

    /* the tree keeps the images it was assembled from for as long as it lives */
    tree = mfr_tree_assemble(app, capture, image_paths, error);
    if (tree == NULL) {
        mfr_composer_release_images(composer, app, MFR_LAYER_COUNT);
        return NULL;
    }
    g_hash_table_insert(composer->trees, g_strdup(component_id), tree);

    return g_object_ref(tree);
}

/**
 * mfr_composer_list_trees:
 * @composer: The composer.
 *
 * Returns: (transfer container) (element-type MfrTree): the cached trees
 */
GList *
mfr_composer_list_trees(MfrComposer *composer)
{
    return g_hash_table_get_values(composer->trees);
}

/**
 * mfr_composer_refresh_capture:
 * @composer: The composer.
 * @error: A #GError
 *
 * Forget every capture, so the next launch captures again. Every tree was
 * made with a capture, so none can stay.
 */
gboolean
mfr_composer_refresh_capture(MfrComposer *composer, GError **error)
{
    mfr_composer_drop_all_trees(composer);
    g_hash_table_remove_all(composer->captures);

    return mfr_capture_discard_all(error);
}
