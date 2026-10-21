/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mfr-image-mounts.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/loop.h>
#include <libcryptsetup.h>
#include <libglnx.h>

#include "mf-store-private.h"

constexpr char DEVICE_PREFIX[] = "manifold-";
constexpr char MAPPER_DIR[] = "/dev/mapper";
constexpr char LOOP_CONTROL[] = "/dev/loop-control";
constexpr size_t ROOT_HASH_SIZE = 32;

/**
 * MfrImageMount:
 *
 * The verified filesystem of an image, mounted below MFR_IMAGES_MOUNT_DIR.
 * A mount is shared between the trees that use it, and counted.
 *
 * An image is a disk image with a filesystem partition, a dm-verity hash
 * partition and a signature partition. The kernel is not asked to look for
 * partitions: the image is attached to a loop device starting at the
 * filesystem partition, dm-verity is told where the hash partition is within
 * that device, and the verified device is mounted below
 * MF_RUN_MOUNT_DIR/images/<hash>/.
 */
typedef struct {
    char *path;
    unsigned int users;
} MfrImageMount;

struct MfrImageMounts {
    GHashTable *table; /* verity hash -> MfrImageMount */
};

/**
 * mfr_image_mount_free:
 */
static void
mfr_image_mount_free(MfrImageMount *img_mount)
{
    g_free(img_mount->path);
    g_free(img_mount);
}

/**
 * mfr_cryptsetup_log:
 */
static void
mfr_cryptsetup_log(int level, const char *message, void *user_data)
{
    g_autofree char *trimmed = g_strchomp(g_strdup(message));

    if (level == CRYPT_LOG_ERROR)
        g_warning("cryptsetup: %s", trimmed);
    else
        g_debug("cryptsetup: %s", trimmed);
}

/**
 * mfr_loop_attach:
 *
 * Attach an image to a loop device, read-only, starting at the given offset.
 * The device detaches itself once nothing uses it anymore.
 *
 * Returns: a descriptor of the loop device, with its path in @ret_path, or -1
 */
static int
mfr_loop_attach(int image_fd, guint64 offset, char **ret_path, GError **error)
{
    glnx_autofd int control_fd = -1;

    control_fd = open(LOOP_CONTROL, O_RDWR | O_CLOEXEC);
    if (control_fd < 0)
        return glnx_throw_errno_prefix(error, "Unable to open %s", LOOP_CONTROL), -1;

    /* someone else may take the free device before we configure it */
    for (int attempt = 0; attempt < 16; attempt++) {
        g_autofree char *path = NULL;
        glnx_autofd int loop_fd = -1;
        struct loop_config config = {
            .fd = (__u32) image_fd,
            .info = {.lo_offset = offset, .lo_flags = LO_FLAGS_READ_ONLY | LO_FLAGS_AUTOCLEAR},
        };

        int number = ioctl(control_fd, LOOP_CTL_GET_FREE);
        if (number < 0)
            return glnx_throw_errno_prefix(error, "Unable to find a free loop device"), -1;

        path = g_strdup_printf("/dev/loop%d", number);
        loop_fd = open(path, O_RDONLY | O_CLOEXEC);
        if (loop_fd < 0)
            return glnx_throw_errno_prefix(error, "Unable to open %s", path), -1;

        if (ioctl(loop_fd, LOOP_CONFIGURE, &config) == 0) {
            *ret_path = g_steal_pointer(&path);
            return g_steal_fd(&loop_fd);
        }
        if (errno != EBUSY)
            return glnx_throw_errno_prefix(error, "Unable to attach image to %s", path), -1;
    }

    return glnx_throw(error, "No loop device stayed free long enough to attach the image to it"), -1;
}

/**
 * mfr_decode_root_hash:
 *
 * Decode a verity root hash from a string.
 */
static gboolean
mfr_decode_root_hash(const char *verity_hash, guint8 *ret_hash)
{
    for (size_t i = 0; i < ROOT_HASH_SIZE; i++) {
        int high = g_ascii_xdigit_value(verity_hash[2 * i]);
        int low = g_ascii_xdigit_value(verity_hash[2 * i + 1]);

        if (high < 0 || low < 0)
            return FALSE;
        ret_hash[i] = (guint8) ((high << 4) | low);
    }

    return TRUE;
}

/**
 * mfr_verity_activate:
 *
 * Set up the verified device for an image that is attached to a loop device.
 */
static gboolean
mfr_verity_activate(const char *loop_path, const MfrLayer *layer, const char *device_name, GError **error)
{
    struct crypt_device *cd = NULL;
    struct crypt_params_verity params = {
        /* the hash partition follows the filesystem partition in the same device */
        .hash_area_offset = layer->hash_offset - layer->data_offset,
    };
    guint8 root_hash[ROOT_HASH_SIZE];
    gboolean ret = FALSE;
    int r;

    if (!mfr_decode_root_hash(layer->verity_hash, root_hash))
        return glnx_throw(error, "Root hash of %s is not valid: %s", layer->component_id, layer->verity_hash);

    r = crypt_init(&cd, loop_path);
    if (r < 0)
        return glnx_throw(error, "Unable to use %s with cryptsetup: %s", loop_path, g_strerror(-r));
    crypt_set_log_callback(cd, mfr_cryptsetup_log, NULL);

    r = crypt_load(cd, CRYPT_VERITY, &params);
    if (r < 0) {
        glnx_throw(error,
                   "Unable to read the verity superblock of %s: %s",
                   layer->component_id,
                   g_strerror(-r));
        goto out;
    }

    /* TODO: the root hash alone is enforced, but the signature partition is
     * not passed to the kernel until repositories sign images */
    r = crypt_activate_by_volume_key(cd,
                                     device_name,
                                     (const char *) root_hash,
                                     sizeof(root_hash),
                                     CRYPT_ACTIVATE_READONLY | CRYPT_ACTIVATE_PRIVATE);
    if (r < 0) {
        glnx_throw(error, "Unable to set up dm-verity for %s: %s", layer->component_id, g_strerror(-r));
        goto out;
    }

    ret = TRUE;
out:
    crypt_free(cd);
    return ret;
}

/**
 * mfr_verity_deactivate:
 */
static gboolean
mfr_verity_deactivate(const char *device_name, gboolean deferred, GError **error)
{
    int r = crypt_deactivate_by_name(NULL, device_name, deferred ? CRYPT_DEACTIVATE_DEFERRED : 0);
    if (r < 0 && r != -ENODEV)
        return glnx_throw(error, "Unable to remove %s: %s", device_name, g_strerror(-r));
    return TRUE;
}

/**
 * mfr_image_open:
 *
 * Open an image by its path in the store. A link at that path is not
 * followed, so no other file can be opened in its place.
 *
 * Returns: a descriptor, or -1
 */
static int
mfr_image_open(const char *path, GError **error)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return glnx_throw_errno_prefix(error, "Unable to open image %s", path), -1;

    return fd;
}

/**
 * mfr_layer_image_setup:
 *
 * Make the verified filesystem of an image available as a block device.
 * A device that exists already is reused: its name is the root hash it
 * enforces, and only root can create one.
 */
static gboolean
mfr_layer_image_setup(const MfrLayer *layer, const char *device_name, const char *device_path, GError **error)
{
    glnx_autofd int image_fd = -1;
    glnx_autofd int loop_fd = -1;
    g_autofree char *loop_path = NULL;

    if (g_file_test(device_path, G_FILE_TEST_EXISTS)) {
        g_debug("Reusing %s for %s", device_path, layer->component_id);
        /* cancel a deferred removal, pending since the last tree using it was dropped */
        crypt_deactivate_by_name(NULL, device_name, CRYPT_DEACTIVATE_DEFERRED_CANCEL);
        return TRUE;
    }

    image_fd = mfr_image_open(layer->image_path, error);
    if (image_fd < 0)
        return FALSE;
    loop_fd = mfr_loop_attach(image_fd, layer->data_offset, &loop_path, error);
    if (loop_fd < 0)
        return FALSE;

    /* if this fails, closing the loop device detaches it again */
    return mfr_verity_activate(loop_path, layer, device_name, error);
}

/**
 * mfr_image_mounts_acquire:
 *
 * Mount an image, or reuse its mount if it is mounted already.
 *
 * Returns: the path the image is mounted at, owned by @mounts
 */
const char *
mfr_image_mounts_acquire(MfrImageMounts *mounts, const MfrLayer *layer, GError **error)
{
    g_autofree char *device_name = NULL;
    g_autofree char *device_path = NULL;
    g_autofree char *path = NULL;

    MfrImageMount *img_mount = g_hash_table_lookup(mounts->table, layer->verity_hash);
    if (img_mount != NULL) {
        img_mount->users++;
        return img_mount->path;
    }

    device_name = g_strconcat(DEVICE_PREFIX, layer->verity_hash, NULL);
    device_path = g_build_filename(MAPPER_DIR, device_name, NULL);
    path = g_build_filename(MFR_IMAGES_MOUNT_DIR, layer->verity_hash, NULL);

    if (!mfr_layer_image_setup(layer, device_name, device_path, error))
        return NULL;
    if (!glnx_shutil_mkdir_p_at(AT_FDCWD, path, 0755, NULL, error))
        goto fail;
    if (mount(device_path, path, "erofs", MS_RDONLY | MS_NOSUID | MS_NODEV, NULL) != 0) {
        glnx_throw_errno_prefix(error, "Unable to mount %s at %s", device_path, path);
        goto fail;
    }
    g_info("Mounted %s %s at %s", layer->component_id, layer->version, path);

    img_mount = g_new0(MfrImageMount, 1);
    img_mount->path = g_steal_pointer(&path);
    img_mount->users = 1;
    g_hash_table_insert(mounts->table, g_strdup(layer->verity_hash), img_mount);

    return img_mount->path;

fail:
    (void) rmdir(path);
    /* deferred, as a device that was reused may still be in use */
    mfr_verity_deactivate(device_name, TRUE, NULL);
    return NULL;
}

/**
 * mfr_image_mounts_release:
 *
 * Let go of an image. With no tree using it anymore, it is unmounted, and its
 * device is removed as soon as no launched application uses it anymore.
 */
void
mfr_image_mounts_release(MfrImageMounts *mounts, const char *verity_hash)
{
    g_autofree char *device_name = g_strconcat(DEVICE_PREFIX, verity_hash, NULL);
    g_autoptr(GError) error = NULL;

    MfrImageMount *img_mount = g_hash_table_lookup(mounts->table, verity_hash);
    g_return_if_fail(img_mount != NULL && img_mount->users > 0);

    if (--img_mount->users > 0)
        return;

    if (umount2(img_mount->path, MNT_DETACH) != 0)
        g_warning("Unable to unmount %s: %s", img_mount->path, g_strerror(errno));
    if (rmdir(img_mount->path) != 0)
        g_warning("Unable to remove %s: %s", img_mount->path, g_strerror(errno));
    if (!mfr_verity_deactivate(device_name, TRUE, &error))
        g_warning("%s", error->message);
    g_debug("Unmounted image %s", verity_hash);

    g_hash_table_remove(mounts->table, verity_hash);
}

/**
 * mfr_image_mounts_new:
 *
 * Create a new, empty table of image mounts.
 *
 * Returns: a new table of image mounts
 */
MfrImageMounts *
mfr_image_mounts_new(void)
{
    MfrImageMounts *mounts = g_new0(MfrImageMounts, 1);

    mounts->table = g_hash_table_new_full(g_str_hash,
                                          g_str_equal,
                                          g_free,
                                          (GDestroyNotify) mfr_image_mount_free);
    return mounts;
}

/**
 * mfr_image_mounts_free:
 */
void
mfr_image_mounts_free(MfrImageMounts *mounts)
{
    if (mounts == NULL)
        return;

    g_hash_table_unref(mounts->table);
    g_free(mounts);
}
