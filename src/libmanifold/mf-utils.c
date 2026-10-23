/*
 * Copyright (C) 2024-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mf-utils.h"
#include "mf-utils-private.h"

#include <string.h>
#include <sys/utsname.h>
#include <glib/gi18n-lib.h>
#include <appstream.h>

#include "mf-error.h"
#include "mf-store-private.h"

constexpr size_t VERSION_LENGTH_MAX = 128;

/* the length of a hex encoded 256 bit hash */
constexpr size_t HEX_HASH_LENGTH = 64;

/**
 * mf_name_is_valid:
 * @name: (nullable): a string
 * @extra_chars: the characters allowed besides letters and digits
 * @length_max: the longest acceptable name
 *
 * Validate that we have a sane name for filesystem use and display,
 * which for us means ASCII letters and digits plus @extra_chars,
 * not starting with `.` or `-`, so that no name is hidden, refers to
 * `.` or `..`, or looks like an option.
 *
 * Returns: %TRUE if @name conforms
 */
gboolean
mf_name_is_valid(const char *name, const char *extra_chars, size_t length_max)
{
    if (name == NULL)
        return FALSE;

    size_t len = strnlen(name, length_max + 1);
    if (len == 0 || len > length_max)
        return FALSE;
    if (name[0] == '.' || name[0] == '-')
        return FALSE;

    for (size_t i = 0; i < len; i++) {
        if (!g_ascii_isalnum(name[i]) && strchr(extra_chars, name[i]) == NULL)
            return FALSE;
    }

    return TRUE;
}

/**
 * mf_version_is_valid:
 * @version: (nullable): a version string
 *
 * Versions are used as directory names, so they are restricted to what a
 * version can reasonably consist of: 1 to 128 characters of
 * `[A-Za-z0-9.+~_-]`, not starting with `.` or `-`. A `:`, as in an epoch,
 * is not among them, and `current` is not a version, as that is the name
 * of the active version of a component in the store.
 *
 * Returns: %TRUE if the version is valid
 */
gboolean
mf_version_is_valid(const char *version)
{
    return mf_name_is_valid(version, ".+~_-", VERSION_LENGTH_MAX) &&
           !g_str_equal(version, MF_STORE_CURRENT_NAME);
}

/**
 * mf_version_compare:
 * @a: a version
 * @b: another version
 *
 * Compare two versions of a component.
 *
 * Returns: a negative number if @a is older than @b, a positive number if
 *   it is newer, 0 if they are the same
 */
int
mf_version_compare(const char *a, const char *b)
{
    return as_vercmp_simple(a, b);
}

/**
 * mf_version_compare_with_rev:
 * @version_a: version a
 * @revision_a: revision of a
 * @version_b: version b
 * @revision_b: revision of b
 *
 * Compare two versions of a component, including their revisions.
 *
 * Returns: a negative number if @a is older than @b, a positive number if
 *   it is newer, 0 if they are the same
 */
int
mf_version_compare_with_rev(const char *version_a,
                            guint64 revision_a,
                            const char *version_b,
                            guint64 revision_b)
{
    int cmp = mf_version_compare(version_a, version_b);

    if (cmp != 0)
        return cmp;
    return revision_a < revision_b ? -1 : revision_a > revision_b ? 1 : 0;
}

/**
 * mf_hex_hash_is_valid:
 * @hash: (nullable): a string
 *
 * Hashes are used as file names, so only lowercase hex of the length of a
 * 256 bit hash is accepted.
 *
 * Returns: %TRUE if @hash is a valid hex hash
 */
gboolean
mf_hex_hash_is_valid(const char *hash)
{
    if (hash == NULL || strnlen(hash, HEX_HASH_LENGTH + 1) != HEX_HASH_LENGTH)
        return FALSE;

    for (size_t i = 0; i < HEX_HASH_LENGTH; i++) {
        if (!g_ascii_isxdigit(hash[i]) || g_ascii_isupper(hash[i]))
            return FALSE;
    }

    return TRUE;
}

/**
 * mf_get_host_architecture:
 *
 * The architecture of the host, as Manifold names it: the CPU part of its
 * multiarch tuple, e.g. `x86_64`, except that 32-bit ARM is named by its
 * ABI, as Debian does: `armhf` (hard-float) or `armel` (soft-float).
 * A CPU can run more than one userland, x86_64 an i386 one, and the kernel
 * does not tell which one is in use, so on x86 and ARM this is the one this
 * library was built for. Elsewhere it is what `uname -m` says.
 *
 * Returns: the architecture of the host
 */
const char *
mf_get_host_architecture(void)
{
#if defined(__x86_64__)
    return "x86_64";
#elif defined(__i386__)
    return "i386";
#elif defined(__arm__) && defined(__ARM_PCS_VFP)
    return "armhf";
#elif defined(__arm__)
    return "armel";
#else
    static char *architecture = NULL;

    if (g_once_init_enter_pointer(&architecture)) {
        struct utsname uts = {0};

        uname(&uts);
        g_once_init_leave_pointer(&architecture, g_strdup(uts.machine));
    }

    return architecture;
#endif
}

/**
 * mf_write_all:
 * @fd: a file descriptor
 * @buffer: a buffer
 * @size: the size of @buffer
 * @error: (nullable): a #GError
 *
 * Writes @size bytes from @buffer to @fd.
 *
 * Returns: %TRUE on success, %FALSE on failure
 */
gboolean
mf_write_all(int fd, const char *buffer, gsize size, GError **error)
{
    while (size > 0) {
        ssize_t n = write(fd, buffer, size);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            g_set_error(error, MF_ERROR, MF_ERROR_FAILED, _("Unable to write: %s"), g_strerror(errno));
            return FALSE;
        }
        buffer += n;
        size -= (gsize) n;
    }
    return TRUE;
}

/**
 * mf_build_full_version:
 * @version: a version
 * @revision: its revision
 *
 * Build the full version string from the version and revision parts.
 * This name is commonly used for directories.
 * It can be split apart again via mf_split_full_version().
 *
 * Returns: (transfer full): the full version string
 */
char *
mf_build_full_version(const char *version, guint64 revision)
{
    return g_strdup_printf("%s-%" G_GUINT64_FORMAT, version, revision);
}

/**
 * mf_split_full_version:
 * @name: (nullable): the name of a version directory, `<version>-<revision>`
 * @ret_version: (out): the version, which is valid
 * @ret_revision: (out): the revision
 *
 * Split the name of a version directory into version and revision. The
 * version may contain `-` itself; the revision is what follows the last one.
 *
 * Returns: %FALSE if @name is not what a version directory is named
 */
gboolean
mf_split_full_version(const char *name, char **ret_version, guint64 *ret_revision)
{
    g_autofree char *version = NULL;
    const char *dash;

    if (name == NULL)
        return FALSE;
    dash = strrchr(name, '-');
    if (dash == NULL || dash == name || dash[1] == '\0')
        return FALSE;
    if (!g_ascii_string_to_unsigned(dash + 1, 10, 0, G_MAXUINT64, ret_revision, NULL))
        return FALSE;
    version = g_strndup(name, (gsize) (dash - name));
    if (!mf_version_is_valid(version))
        return FALSE;

    *ret_version = g_steal_pointer(&version);
    return TRUE;
}
