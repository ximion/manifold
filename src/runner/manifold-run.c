/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define _GNU_SOURCE

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/capability.h>
#include <systemd/sd-json.h>
#include <systemd/sd-varlink.h>

#include "mfr-run-interface.h"

/*
 * manifold-run is the runner that users call. It asks the compose service
 * for the mount tree of a component, and launches the component in it.
 *
 * This tool is unprivileged and runs as the invoking user. It moves its own
 * process into the mount tree and executes the application in place.
 *
 * Changing the mount namespace of a process needs CAP_SYS_ADMIN. If we
 * already have that (e.g. if we were executed by a systemd service), we just
 * move into the tree directly, and keep everything we started with. If we
 * don't have capabilities, we create a user namespace first.
 */

#define PROGRAM_NAME "manifold-run"

typedef struct {
    int mount_fd;
    char *executable;   /* absolute path within the tree */
    char **environment; /* NAME=VALUE, an empty value means unset */
} MfxLaunchData;

typedef struct {
    unsigned int mount_fd_index;
    char *executable;
    char **environment;
} MfxPrepareLaunchReply;

/**
 * mfx_print_usage:
 */
static void
mfx_print_usage(FILE *stream)
{
    fprintf(stream,
            "Usage: %s [OPTIONS] <component-id> [args...]\n"
            "\n"
            "Options:\n"
            "  --socket=PATH      Connect to the compose service at PATH\n"
            "                     (default: %s)\n"
            "  --command=PROGRAM  Run PROGRAM in the environment of the component,\n"
            "                     instead of the component itself\n"
            "  --help             Show this help\n"
            "  --version          Show the program version\n",
            PROGRAM_NAME,
            MF_RUN_SOCKET_PATH);
}

/**
 * mfx_print_connection_error:
 */
static void
mfx_print_connection_error(const char *socket_path, int negative_errno)
{
    fprintf(stderr,
            "%s: Unable to reach the Manifold compose service at %s: %s\n",
            PROGRAM_NAME,
            socket_path,
            strerror(-negative_errno));
}

/**
 * mfx_print_service_error:
 */
static void
mfx_print_service_error(const char *component_id, const char *error_id, sd_json_variant *parameters)
{
    if (strcmp(error_id, MF_RUN_ERROR_NO_SUCH_COMPONENT_ID) == 0) {
        fprintf(stderr, "%s: Component %s is not installed\n", PROGRAM_NAME, component_id);
    } else if (strcmp(error_id, MF_RUN_ERROR_COMPONENT_REVOKED_ID) == 0) {
        fprintf(stderr, "%s: Component %s was revoked and can not be launched\n", PROGRAM_NAME, component_id);
    } else if (strcmp(error_id, MF_RUN_ERROR_COMPOSITION_FAILED_ID) == 0) {
        sd_json_variant *message = sd_json_variant_by_key(parameters, "message");

        fprintf(stderr,
                "%s: Unable to prepare component %s: %s\n",
                PROGRAM_NAME,
                component_id,
                sd_json_variant_is_string(message) ? sd_json_variant_string(message) : "unknown error");
    } else if (strcmp(error_id, SD_VARLINK_ERROR_INVALID_PARAMETER) == 0) {
        fprintf(stderr, "%s: Component identifier is not valid: %s\n", PROGRAM_NAME, component_id);
    } else if (strcmp(error_id, SD_VARLINK_ERROR_PERMISSION_DENIED) == 0) {
        fprintf(stderr, "%s: Not permitted to launch component %s\n", PROGRAM_NAME, component_id);
    } else {
        fprintf(stderr,
                "%s: The Manifold compose service failed to prepare component %s: %s\n",
                PROGRAM_NAME,
                component_id,
                error_id);
    }
}

/**
 * mfx_prepare_launch:
 *
 * Ask the compose service for everything we need to launch a component.
 *
 * Returns: 0 on success, or a negative value once we printed why we failed
 */
static int
mfx_prepare_launch(const char *socket_path, const char *component_id, MfxLaunchData *launch_data)
{
    static const sd_json_dispatch_field dispatch_table[] = {
        {"mountFileDescriptor",
         _SD_JSON_VARIANT_TYPE_INVALID, sd_json_dispatch_uint,
         offsetof(MfxPrepareLaunchReply, mount_fd_index),
         SD_JSON_MANDATORY},
        {"executable",
         SD_JSON_VARIANT_STRING, sd_json_dispatch_string,
         offsetof(MfxPrepareLaunchReply, executable),
         SD_JSON_MANDATORY},
        {"environment",
         SD_JSON_VARIANT_ARRAY, sd_json_dispatch_strv,
         offsetof(MfxPrepareLaunchReply, environment),
         0},
        {},
    };
    MfxPrepareLaunchReply reply = {};
    sd_varlink *link = nullptr;
    sd_json_variant *parameters = nullptr;
    const char *error_id = nullptr;
    int mount_fd = -1;
    int r;

    r = sd_varlink_connect_address(&link, socket_path);
    if (r < 0) {
        mfx_print_connection_error(socket_path, r);
        goto out;
    }

    r = sd_varlink_set_allow_fd_passing_input(link, true);
    if (r < 0) {
        fprintf(stderr, "%s: Unable to allow receiving file descriptors: %s\n", PROGRAM_NAME, strerror(-r));
        goto out;
    }

    r = sd_varlink_callbo(link,
                          MF_RUN_METHOD_PREPARE_LAUNCH,
                          &parameters,
                          &error_id,
                          SD_JSON_BUILD_PAIR_STRING("componentId", component_id));
    if (r < 0) {
        mfx_print_connection_error(socket_path, r);
        goto out;
    }
    if (error_id != nullptr) {
        mfx_print_service_error(component_id, error_id, parameters);
        r = -EIO;
        goto out;
    }

    r = sd_json_dispatch(parameters, dispatch_table, SD_JSON_ALLOW_EXTENSIONS, &reply);
    if (r < 0) {
        fprintf(stderr,
                "%s: Failed to parse reply of the Manifold compose service: %s\n",
                PROGRAM_NAME,
                strerror(-r));
        goto out;
    }

    if (reply.executable[0] != '/') {
        fprintf(stderr,
                "%s: Received a relative executable path (this is not allowed): %s\n",
                PROGRAM_NAME,
                reply.executable);
        r = -EINVAL;
        goto out;
    }

    mount_fd = sd_varlink_take_fd(link, reply.mount_fd_index);
    if (mount_fd < 0) {
        fprintf(stderr, "%s: Did not receive a mount tree\n", PROGRAM_NAME);
        r = -EBADF;
        goto out;
    }

    launch_data->mount_fd = mount_fd;
    launch_data->executable = reply.executable;
    launch_data->environment = reply.environment;
    reply.executable = nullptr;
    reply.environment = nullptr;
    mount_fd = -1;
    r = 0;

out:
    if (mount_fd >= 0)
        close(mount_fd);
    free(reply.executable);
    if (reply.environment != nullptr) {
        for (char **e = reply.environment; *e != nullptr; e++)
            free(*e);
        free(reply.environment);
    }
    /* the reply is owned by the connection */
    sd_varlink_close_unref(link);

    return r;
}

/**
 * mfx_have_cap_sys_admin:
 *
 * Check if our current process has CAP_SYS_ADMIn
 */
static bool
mfx_have_cap_sys_admin(void)
{
    struct __user_cap_header_struct header = {.version = _LINUX_CAPABILITY_VERSION_3};
    struct __user_cap_data_struct data[_LINUX_CAPABILITY_U32S_3] = {};

    if (syscall(SYS_capget, &header, data) != 0)
        return false;

    return (data[CAP_TO_INDEX(CAP_SYS_ADMIN)].effective & CAP_TO_MASK(CAP_SYS_ADMIN)) != 0;
}

/**
 * mfx_write_proc_file:
 *
 * Write a string to a file in /proc/self
 */
static int
mfx_write_proc_file(const char *path, const char *content)
{
    int fd;
    ssize_t written;

    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    written = write(fd, content, strlen(content));
    if (written < 0) {
        int saved_errno = errno;

        close(fd);
        return -saved_errno;
    }
    close(fd);

    return 0;
}

/**
 * mfx_map_self:
 *
 * Map our uid and gid to themselves in the new user namespace; no other
 * IDs are mapped.
 */
static int
mfx_map_self(uid_t uid, gid_t gid)
{
    char mapping[64];
    int r;

    r = mfx_write_proc_file("/proc/self/setgroups", "deny");
    if (r < 0)
        return r;
    snprintf(mapping, sizeof(mapping), "%u %u 1", (unsigned int) uid, (unsigned int) uid);
    r = mfx_write_proc_file("/proc/self/uid_map", mapping);
    if (r < 0)
        return r;
    snprintf(mapping, sizeof(mapping), "%u %u 1", (unsigned int) gid, (unsigned int) gid);
    return mfx_write_proc_file("/proc/self/gid_map", mapping);
}

/**
 * mfx_enter_tree:
 *
 * Make the mount tree the root of a new mount namespace of ours. In a user
 * namespace, we have the capability to do so only inside it.
 *
 * Returns: 0 on success, a negative value on failure, with a message printed.
 */
static int
mfx_enter_tree(int mount_fd, bool user_namespace)
{
    int flags = CLONE_NEWNS | (user_namespace ? CLONE_NEWUSER : 0);
    /* our IDs outside of the new namespace; inside it, we are nobody until we map them */
    uid_t uid = getuid();
    gid_t gid = getgid();
    int r;

    if (unshare(flags) != 0) {
        r = -errno;
        if (user_namespace)
            fprintf(stderr,
                    "%s: Unable to create a user namespace: %s"
                    " (unprivileged user namespaces may be disabled on this host)\n",
                    PROGRAM_NAME,
                    strerror(-r));
        else
            fprintf(stderr, "%s: Unable to create a mount namespace: %s\n", PROGRAM_NAME, strerror(-r));
        return r;
    }

    if (user_namespace) {
        r = mfx_map_self(uid, gid);
        if (r < 0) {
            fprintf(stderr,
                    "%s: Unable to map our user into the user namespace: %s\n",
                    PROGRAM_NAME,
                    strerror(-r));
            return r;
        }
    }

    /* nothing we do from here on must propagate back to the host */
    if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
        r = -errno;
        fprintf(stderr, "%s: Unable to make mounts private: %s\n", PROGRAM_NAME, strerror(-r));
        return r;
    }
    if (move_mount(mount_fd, "", AT_FDCWD, "/", MOVE_MOUNT_F_EMPTY_PATH) != 0) {
        r = -errno;
        fprintf(stderr, "%s: Unable to attach the mount tree: %s\n", PROGRAM_NAME, strerror(-r));
        return r;
    }
    if (fchdir(mount_fd) != 0) {
        r = -errno;
        fprintf(stderr, "%s: Unable to change into the mount tree: %s\n", PROGRAM_NAME, strerror(-r));
        return r;
    }
    /* stacks the old root on top of the new one, so we can detach it via "." */
    if (syscall(SYS_pivot_root, ".", ".") != 0) {
        r = -errno;
        fprintf(stderr, "%s: Unable to pivot into the mount tree: %s\n", PROGRAM_NAME, strerror(-r));
        return r;
    }
    if (umount2(".", MNT_DETACH) != 0) {
        r = -errno;
        fprintf(stderr, "%s: Unable to detach the old root: %s\n", PROGRAM_NAME, strerror(-r));
        return r;
    }

    return 0;
}

/**
 * mfx_change_directory:
 *
 * Stay where the caller was, if that exists in the tree; otherwise go to
 * the caller's home, or to the root of the tree.
 */
static void
mfx_change_directory(const char *cwd)
{
    if (cwd != nullptr && chdir(cwd) == 0)
        return;

    const char *home = getenv("HOME");
    if (home != nullptr && chdir(home) == 0)
        return;
    if (chdir("/") != 0)
        fprintf(stderr, "%s: Unable to change to the root of the tree: %s\n", PROGRAM_NAME, strerror(errno));
}

/**
 * mfx_apply_environment:
 *
 * Apply the environment variables from the caller.
 */
static void
mfx_apply_environment(char **environment)
{
    if (environment == nullptr)
        return;
    for (char **e = environment; *e != nullptr; e++) {
        char *separator = strchr(*e, '=');

        if (separator == nullptr)
            continue;
        *separator = '\0';
        if (separator[1] == '\0')
            unsetenv(*e);
        else
            setenv(*e, separator + 1, 1);
    }
}

/**
 * mfx_execute_command:
 *
 * Execute a program the caller named, which is in the tree: by its path, or
 * by its name. We look in /app/bin first, and then in the caller's PATH.
 */
static void
mfx_execute_command(const char *command, char **args)
{
    const char *path_env = getenv("PATH");
    char *search = nullptr;
    char *saveptr = nullptr;

    args[0] = (char *) command;
    if (strchr(command, '/') != nullptr) {
        execv(command, args);
        fprintf(stderr, "%s: Unable to execute %s: %s\n", PROGRAM_NAME, command, strerror(errno));
        return;
    }

    int r = asprintf(&search, "/app/bin:%s", path_env != nullptr ? path_env : "/usr/local/bin:/usr/bin:/bin");
    if (r < 0) {
        fprintf(stderr, "%s: Out of memory\n", PROGRAM_NAME);
        return;
    }
    for (const char *dir = strtok_r(search, ":", &saveptr); dir != nullptr;
         dir = strtok_r(nullptr, ":", &saveptr)) {
        char path[PATH_MAX];

        if (dir[0] != '/')
            continue;
        if (snprintf(path, sizeof(path), "%s/%s", dir, command) >= (int) sizeof(path))
            continue;
        if (access(path, X_OK) != 0)
            continue;
        execv(path, args);
        fprintf(stderr, "%s: Unable to execute %s: %s\n", PROGRAM_NAME, path, strerror(errno));
        free(search);
        return;
    }

    fprintf(stderr, "%s: There is no program %s in the component\n", PROGRAM_NAME, command);
    free(search);
}

/**
 * main:
 */
int
main(int argc, char **argv)
{
    static const struct option long_options[] = {
        {"socket",  required_argument, nullptr, 's'},
        {"command", required_argument, nullptr, 'c'},
        {"help",    no_argument,       nullptr, 'h'},
        {"version", no_argument,       nullptr, 'v'},
        {nullptr,   0,                 nullptr, 0  },
    };
    const char *socket_path = MF_RUN_SOCKET_PATH;
    const char *command = nullptr;
    char cwd[PATH_MAX];
    MfxLaunchData launch_data = {.mount_fd = -1};
    int opt;

    /* the leading "+" stops parsing at the first non-option argument */
    while ((opt = getopt_long(argc, argv, "+", long_options, nullptr)) != -1) {
        switch (opt) {
        case 's':
            socket_path = optarg;
            break;
        case 'c':
            command = optarg;
            break;
        case 'h':
            mfx_print_usage(stdout);
            return EXIT_SUCCESS;
        case 'v':
            printf(PROGRAM_NAME " " MF_VERSION "\n");
            return EXIT_SUCCESS;
        default:
            mfx_print_usage(stderr);
            return EXIT_FAILURE;
        }
    }

    if (argc - optind < 1) {
        mfx_print_usage(stderr);
        return EXIT_FAILURE;
    }

    /* everything after the component identifier is an argument of the application */
    const char *component_id = argv[optind];

    if (mfx_prepare_launch(socket_path, component_id, &launch_data) < 0)
        return EXIT_FAILURE;

    if (getcwd(cwd, sizeof(cwd)) == nullptr)
        cwd[0] = '\0';
    if (mfx_enter_tree(launch_data.mount_fd, !mfx_have_cap_sys_admin()) < 0)
        return EXIT_FAILURE;
    close(launch_data.mount_fd);

    mfx_change_directory(cwd[0] != '\0' ? cwd : nullptr);
    mfx_apply_environment(launch_data.environment);

    /* argv[optind] is replaced by the program, what follows are its arguments */
    mfx_execute_command(command != nullptr ? command : launch_data.executable, &argv[optind]);
    return EXIT_FAILURE;
}
