/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mf-client.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <systemd/sd-json.h>
#include <systemd/sd-varlink.h>

#include "mf-component-private.h"
#include "mf-interface-private.h"
#include "mf-progress-private.h"
#include "mf-result-private.h"

/**
 * MfClient:
 *
 * Interface to the Manifold service, to manage the local Manifold installation.
 *
 * Every call connects to the system Manifold service, so a client does not hold a
 * connection while it is not used, and the service may exit in between.
 */

/* installing a bundle takes as long as it takes */
constexpr uint64_t CALL_TIMEOUT_USEC = UINT64_MAX;

typedef struct {
    char *socket_path;
} MfClientPrivate;

G_DEFINE_TYPE_WITH_PRIVATE(MfClient, mf_client, G_TYPE_OBJECT)

#define GET_PRIVATE(o) ((MfClientPrivate *) mf_client_get_instance_private(o))

static void
mf_client_finalize(GObject *object)
{
    MfClientPrivate *priv = GET_PRIVATE(MF_CLIENT(object));

    g_free(priv->socket_path);

    G_OBJECT_CLASS(mf_client_parent_class)->finalize(object);
}

static void
mf_client_class_init(MfClientClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = mf_client_finalize;
}

static void
mf_client_init(MfClient *client)
{
    GET_PRIVATE(client)->socket_path = g_strdup(MF_SOCKET_PATH);
}

/**
 * mf_client_new:
 *
 * Create a client of the Manifold service.
 *
 * Returns: (transfer full): a new #MfClient
 */
MfClient *
mf_client_new(void)
{
    return g_object_new(MF_TYPE_CLIENT, NULL);
}

/**
 * mf_client_get_socket_path:
 * @client: an #MfClient
 *
 * Returns: the path of the socket the service is reached at
 */
const char *
mf_client_get_socket_path(MfClient *client)
{
    g_return_val_if_fail(MF_IS_CLIENT(client), NULL);
    return GET_PRIVATE(client)->socket_path;
}

/**
 * mf_client_set_socket_path:
 * @client: an #MfClient
 * @socket_path: path of a socket
 *
 * Reach the service at a different socket than the default one.
 */
void
mf_client_set_socket_path(MfClient *client, const char *socket_path)
{
    MfClientPrivate *priv;

    g_return_if_fail(MF_IS_CLIENT(client));
    g_return_if_fail(socket_path != NULL);

    priv = GET_PRIVATE(client);
    g_free(priv->socket_path);
    priv->socket_path = g_strdup(socket_path);
}

static const gchar *
mf_parameter_string(sd_json_variant *parameters, const gchar *key, const gchar *fallback)
{
    sd_json_variant *value = sd_json_variant_by_key(parameters, key);

    return sd_json_variant_is_string(value) ? sd_json_variant_string(value) : fallback;
}

/**
 * mf_set_error_from_service:
 *
 * Turn an error the service replied with into a #GError that says what
 * happened, without the caller having to know the service.
 */
static void
mf_set_error_from_service(GError **error, const gchar *error_id, sd_json_variant *parameters)
{
    const gchar *message = mf_parameter_string(parameters, "message", "unknown error");

    if (g_strcmp0(error_id, MF_VARLINK_ERROR_INVALID_BUNDLE) == 0) {
        g_set_error(error, MF_ERROR, MF_ERROR_INVALID_BUNDLE, "Not a valid bundle: %s", message);
    } else if (g_strcmp0(error_id, MF_VARLINK_ERROR_VERIFICATION_FAILED) == 0) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_VERIFICATION_FAILED,
                    "The bundle could not be verified: %s",
                    message);
    } else if (g_strcmp0(error_id, MF_VARLINK_ERROR_MISSING_DEPENDENCY) == 0) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_MISSING_DEPENDENCY,
                    "Component %s needs the %s %s %s, which is not installed",
                    mf_parameter_string(parameters, "componentId", "?"),
                    mf_parameter_string(parameters, "kind", "component"),
                    mf_parameter_string(parameters, "dependencyId", "?"),
                    mf_parameter_string(parameters, "dependencyVersion", "?"));
    } else if (g_strcmp0(error_id, MF_VARLINK_ERROR_ALREADY_INSTALLED) == 0) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_ALREADY_INSTALLED,
                    "Component %s %s is installed already",
                    mf_parameter_string(parameters, "componentId", "?"),
                    mf_parameter_string(parameters, "version", "?"));
    } else if (g_strcmp0(error_id, MF_VARLINK_ERROR_NO_SUCH_COMPONENT) == 0) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_NO_SUCH_COMPONENT,
                    "Component %s is not installed",
                    mf_parameter_string(parameters, "componentId", "?"));
    } else if (g_strcmp0(error_id, MF_VARLINK_ERROR_COMPONENT_IN_USE) == 0) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_COMPONENT_IN_USE,
                    "Component %s is used by the installed component %s",
                    mf_parameter_string(parameters, "componentId", "?"),
                    mf_parameter_string(parameters, "usedBy", "?"));
    } else if (g_strcmp0(error_id, SD_VARLINK_ERROR_PERMISSION_DENIED) == 0) {
        g_set_error_literal(error,
                            MF_ERROR,
                            MF_ERROR_PERMISSION_DENIED,
                            "Not permitted to change the installed components");
    } else if (g_strcmp0(error_id, SD_VARLINK_ERROR_METHOD_NOT_IMPLEMENTED) == 0 ||
               g_strcmp0(error_id, SD_VARLINK_ERROR_METHOD_NOT_FOUND) == 0) {
        g_set_error_literal(error,
                            MF_ERROR,
                            MF_ERROR_NOT_IMPLEMENTED,
                            "The Manifold service does not implement this operation");
    } else if (g_strcmp0(error_id, SD_VARLINK_ERROR_INVALID_PARAMETER) == 0) {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_FAILED,
                    "The Manifold service rejected the parameter %s",
                    mf_parameter_string(parameters, "parameter", "?"));
    } else if (g_strcmp0(error_id, MF_VARLINK_ERROR_FAILED) == 0) {
        g_set_error_literal(error, MF_ERROR, MF_ERROR_FAILED, message);
    } else {
        g_set_error(error,
                    MF_ERROR,
                    MF_ERROR_FAILED,
                    "The Manifold service replied with an error: %s",
                    error_id);
    }
}

typedef struct {
    sd_varlink *link;
    GSource *source; /* owned by the main context */
    MfProgressFunc progress_func;
    gpointer progress_data;
    sd_json_variant *reply; /* the final reply */
    GError *error;
    gboolean done;
} MfCall;

/**
 * mf_call_free:
 */
static void
mf_call_free(MfCall *call)
{
    if (call->source != NULL)
        g_source_destroy(call->source);
    sd_varlink_close_unref(call->link);
    sd_json_variant_unref(call->reply);
    g_clear_error(&call->error);
    g_free(call);
}

/**
 * mf_call_report_progress:
 *
 * Pass a progress reply on to the caller.
 */
static void
mf_call_report_progress(MfCall *call, sd_json_variant *variant)
{
    g_autoptr(MfProgress) progress = mf_progress_new_from_reply(variant);

    if (progress != NULL)
        call->progress_func(progress, call->progress_data);
}

/**
 * mf_varlink_on_reply:
 *
 * Handle one reply of the service: progress, the final reply, or an error.
 */
static int
mf_varlink_on_reply(sd_varlink *link,
                    sd_json_variant *parameters,
                    const char *error_id,
                    sd_varlink_reply_flags_t flags,
                    void *userdata)
{
    MfCall *call = userdata;
    sd_json_variant *progress;

    if (error_id != NULL) {
        mf_set_error_from_service(&call->error, error_id, parameters);
        call->done = TRUE;
        return 0;
    }

    progress = sd_json_variant_by_key(parameters, "progress");
    if (call->progress_func != NULL && sd_json_variant_is_object(progress))
        mf_call_report_progress(call, progress);

    if (!(flags & SD_VARLINK_REPLY_CONTINUES)) {
        call->reply = sd_json_variant_ref(parameters);
        call->done = TRUE;
    }

    return 0;
}

/**
 * MfCallSource:
 * The source that drives a connection from a GLib main context: it waits
 * for what sd-varlink wants to wait for, and lets it process.
 */
typedef struct {
    GSource source;
    GTask *task;
    gpointer fd_tag;
    GIOCondition condition;
    uint64_t timeout_usec;
} MfCallSource;

/**
 * mf_call_source_prepare:
 */
static gboolean
mf_call_source_prepare(GSource *source, int *timeout_ms)
{
    MfCallSource *call_source = (MfCallSource *) source;
    MfCall *call = g_task_get_task_data(call_source->task);
    GIOCondition condition = G_IO_HUP | G_IO_ERR;

    int events = sd_varlink_get_events(call->link);
    if (events > 0) {
        if (events & POLLIN)
            condition |= G_IO_IN;
        if (events & POLLOUT)
            condition |= G_IO_OUT;
    }
    if (condition != call_source->condition) {
        g_source_modify_unix_fd(source, call_source->fd_tag, condition);
        call_source->condition = condition;
    }

    if (sd_varlink_get_timeout(call->link, &call_source->timeout_usec) < 0)
        call_source->timeout_usec = UINT64_MAX;
    if (call_source->timeout_usec == UINT64_MAX) {
        *timeout_ms = -1;
    } else {
        uint64_t now = (uint64_t) g_get_monotonic_time();

        *timeout_ms = call_source->timeout_usec <= now
                          ? 0
                          : (int) MIN((call_source->timeout_usec - now) / 1000 + 1, G_MAXINT);
    }

    return *timeout_ms == 0;
}

/**
 * mf_call_source_check:
 */
static gboolean
mf_call_source_check(GSource *source)
{
    MfCallSource *call_source = (MfCallSource *) source;

    if (g_source_query_unix_fd(source, call_source->fd_tag) != 0)
        return TRUE;
    return call_source->timeout_usec != UINT64_MAX &&
           (uint64_t) g_get_monotonic_time() >= call_source->timeout_usec;
}

static void
mf_json_variant_ptr_unref(gpointer variant)
{
    sd_json_variant_unref(variant);
}

/**
 * mf_call_source_dispatch:
 */
static gboolean
mf_call_source_dispatch(GSource *source, GSourceFunc callback, gpointer user_data)
{
    MfCallSource *call_source = (MfCallSource *) source;
    GTask *task = call_source->task;
    MfCall *call = g_task_get_task_data(task);
    int r;

    if (g_task_return_error_if_cancelled(task))
        return G_SOURCE_REMOVE;

    do {
        r = sd_varlink_process(call->link);
    } while (r > 0 && !call->done);

    if (r < 0) {
        g_task_return_new_error(task,
                                MF_ERROR,
                                MF_ERROR_NO_SERVICE,
                                "Lost the Manifold service: %s",
                                g_strerror(-r));
        return G_SOURCE_REMOVE;
    }
    if (!call->done)
        return G_SOURCE_CONTINUE;

    if (call->error != NULL)
        g_task_return_error(task, g_steal_pointer(&call->error));
    else
        g_task_return_pointer(task, g_steal_pointer(&call->reply), mf_json_variant_ptr_unref);
    return G_SOURCE_REMOVE;
}

/**
 * mf_call_source_finalize:
 */
static void
mf_call_source_finalize(GSource *source)
{
    MfCallSource *call_source = (MfCallSource *) source;
    MfCall *call = g_task_get_task_data(call_source->task);

    /* the source is gone, the call must not destroy it again */
    if (call != NULL)
        call->source = NULL;
    g_object_unref(call_source->task);
}

static GSourceFuncs mf_call_source_funcs = {
    .prepare = mf_call_source_prepare,
    .check = mf_call_source_check,
    .dispatch = mf_call_source_dispatch,
    .finalize = mf_call_source_finalize,
};

/**
 * mf_client_call_async:
 * @fd: a file descriptor to pass along with the call, or -1
 * @more: whether the method reports progress in replies before the final one
 *
 * Connect to the service and call a method. The task completes with the
 * final reply, as an sd_json_variant.
 */
static void
mf_client_call_async(MfClient *client,
                     const char *method,
                     sd_json_variant *parameters,
                     int fd,
                     gboolean more,
                     MfProgressFunc progress_func,
                     gpointer progress_data,
                     GCancellable *cancellable,
                     GAsyncReadyCallback callback,
                     gpointer user_data,
                     gpointer source_tag)
{
    g_autoptr(GTask) task = g_task_new(client, cancellable, callback, user_data);
    MfCallSource *call_source;

    g_task_set_source_tag(task, source_tag);

    MfCall *call = g_new0(MfCall, 1);
    call->progress_func = progress_func;
    call->progress_data = progress_data;
    g_task_set_task_data(task, call, (GDestroyNotify) mf_call_free);

    int r = sd_varlink_connect_address(&call->link, GET_PRIVATE(client)->socket_path);
    if (r < 0) {
        g_task_return_new_error(task,
                                MF_ERROR,
                                MF_ERROR_NO_SERVICE,
                                "Unable to reach the Manifold service at %s: %s",
                                GET_PRIVATE(client)->socket_path,
                                g_strerror(-r));
        return;
    }
    sd_varlink_set_userdata(call->link, call);
    r = sd_varlink_set_relative_timeout(call->link, CALL_TIMEOUT_USEC);
    if (r >= 0)
        r = sd_varlink_bind_reply(call->link, mf_varlink_on_reply);
    if (r < 0) {
        g_task_return_new_error(task,
                                MF_ERROR,
                                MF_ERROR_FAILED,
                                "Unable to set up the connection to the Manifold service: %s",
                                g_strerror(-r));
        return;
    }

    if (fd >= 0) {
        /* the connection takes ownership of the descriptor it sends */
        int fd_copy = fcntl(fd, F_DUPFD_CLOEXEC, 3);

        if (fd_copy < 0) {
            g_task_return_new_error(task,
                                    MF_ERROR,
                                    MF_ERROR_FAILED,
                                    "Unable to duplicate file descriptor: %s",
                                    g_strerror(errno));
            return;
        }
        r = sd_varlink_set_allow_fd_passing_output(call->link, true);
        if (r >= 0)
            r = sd_varlink_push_fd(call->link, fd_copy);
        if (r < 0) {
            close(fd_copy);
            g_task_return_new_error(task,
                                    MF_ERROR,
                                    MF_ERROR_FAILED,
                                    "Unable to pass file descriptor: %s",
                                    g_strerror(-r));
            return;
        }
    }

    r = more ? sd_varlink_observe(call->link, method, parameters)
             : sd_varlink_invoke(call->link, method, parameters);
    if (r < 0) {
        g_task_return_new_error(task,
                                MF_ERROR,
                                MF_ERROR_NO_SERVICE,
                                "Unable to call the Manifold service: %s",
                                g_strerror(-r));
        return;
    }

    GSource *source = g_source_new(&mf_call_source_funcs, sizeof(MfCallSource));
    call_source = (MfCallSource *) source;
    call_source->task = g_object_ref(task);
    call_source->condition = G_IO_IN | G_IO_HUP | G_IO_ERR;
    call_source->fd_tag = g_source_add_unix_fd(source, sd_varlink_get_fd(call->link), call_source->condition);
    call_source->timeout_usec = UINT64_MAX;
    g_source_set_static_name(source, "[manifold] call");
    if (cancellable != NULL) {
        g_autoptr(GSource) cancel_source = g_cancellable_source_new(cancellable);

        g_source_set_dummy_callback(cancel_source);
        g_source_add_child_source(source, cancel_source);
    }
    call->source = source;
    g_source_attach(source, g_main_context_get_thread_default());
    g_source_unref(source);
}

/**
 * mf_client_call_finish:
 *
 * Returns: (transfer full): the final reply of a call, or %NULL on failure
 */
static sd_json_variant *
mf_client_call_finish(MfClient *client, GAsyncResult *result, gpointer source_tag, GError **error)
{
    g_return_val_if_fail(g_task_is_valid(result, client), NULL);
    g_return_val_if_fail(g_async_result_is_tagged(result, source_tag), NULL);

    return g_task_propagate_pointer(G_TASK(result), error);
}

/**
 * mf_client_result_finish:
 *
 * Returns: (transfer full): what the operation did, or %NULL on error
 */
static MfResult *
mf_client_result_finish(MfClient *client, GAsyncResult *result, gpointer source_tag, GError **error)
{
    sd_json_variant *reply = mf_client_call_finish(client, result, source_tag, error);
    if (reply == NULL)
        return NULL;

    MfResult *ret = mf_result_new_from_reply(reply, error);
    sd_json_variant_unref(reply);
    return ret;
}

static void
mf_set_build_parameters_error(GError **error, int negative_errno)
{
    g_set_error(error,
                MF_ERROR,
                MF_ERROR_FAILED,
                "Unable to build call parameters: %s",
                g_strerror(-negative_errno));
}

/**
 * mf_client_return_error_async:
 *
 * Fail a call before it was made.
 */
static void
mf_client_return_error_async(MfClient *client,
                             GCancellable *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer user_data,
                             gpointer source_tag,
                             GError *error)
{
    g_autoptr(GTask) task = g_task_new(client, cancellable, callback, user_data);

    g_task_set_source_tag(task, source_tag);
    g_task_return_error(task, error);
}

/**
 * MfSyncCall:
 * An asynchronous call, waited for in a main context of its own.
 */
typedef struct {
    GMainContext *context;
    GAsyncResult *result;
} MfSyncCall;

/**
 * mf_sync_call_begin:
 */
static void
mf_sync_call_begin(MfSyncCall *sync)
{
    sync->context = g_main_context_new();
    g_main_context_push_thread_default(sync->context);
}

/**
 * mf_on_sync_call_done:
 */
static void
mf_on_sync_call_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
    MfSyncCall *sync = user_data;

    sync->result = g_object_ref(result);
}

/**
 * mf_sync_call_wait:
 */
static GAsyncResult *
mf_sync_call_wait(MfSyncCall *sync)
{
    while (sync->result == NULL)
        g_main_context_iteration(sync->context, TRUE);
    return sync->result;
}

/**
 * mf_sync_call_end:
 */
static void
mf_sync_call_end(MfSyncCall *sync)
{
    g_clear_object(&sync->result);
    if (sync->context != NULL) {
        g_main_context_pop_thread_default(sync->context);
        g_clear_pointer(&sync->context, g_main_context_unref);
    }
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(MfSyncCall, mf_sync_call_end)

/**
 * mf_client_install_file_async:
 * @client: an #MfClient
 * @fd: file descriptor of a bundle, opened for reading
 * @progress_func: (nullable) (scope notified): called whenever the service reports progress
 * @progress_data: passed to @progress_func
 * @cancellable: (nullable): a #GCancellable
 * @callback: called when the installation is done, or failed
 * @user_data: passed to @callback
 *
 * Install the components of a bundle. The service reads the bundle through
 * the file descriptor, so it installs exactly the file the caller opened.
 */
void
mf_client_install_file_async(MfClient *client,
                             int fd,
                             MfProgressFunc progress_func,
                             gpointer progress_data,
                             GCancellable *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer user_data)
{
    sd_json_variant *parameters = NULL;

    g_return_if_fail(MF_IS_CLIENT(client));
    g_return_if_fail(fd >= 0);

    /* the descriptor is the first and only one passed along */
    int r = sd_json_buildo(&parameters, SD_JSON_BUILD_PAIR_INTEGER("bundleFileDescriptor", 0));
    if (r < 0) {
        GError *error = NULL;

        mf_set_build_parameters_error(&error, r);
        mf_client_return_error_async(client,
                                     cancellable,
                                     callback,
                                     user_data,
                                     mf_client_install_file_async,
                                     error);
        return;
    }

    mf_client_call_async(client,
                         MF_VARLINK_METHOD_INSTALL_FILE,
                         parameters,
                         fd,
                         TRUE,
                         progress_func,
                         progress_data,
                         cancellable,
                         callback,
                         user_data,
                         mf_client_install_file_async);
    sd_json_variant_unref(parameters);
}

/**
 * mf_client_install_fd_finish:
 * @client: an #MfClient
 * @result: the result passed to the callback
 * @error: a #GError
 *
 * Returns: (transfer full): what was installed, or %NULL on failure
 */
MfResult *
mf_client_install_file_finish(MfClient *client, GAsyncResult *result, GError **error)
{
    return mf_client_result_finish(client, result, mf_client_install_file_async, error);
}

/**
 * mf_client_install_fd:
 * @client: an #MfClient
 * @fd: file descriptor of a bundle, opened for reading
 * @progress_func: (nullable) (scope call): called whenever the service reports progress
 * @progress_data: passed to @progress_func
 * @cancellable: (nullable): a #GCancellable
 * @error: a #GError
 *
 * Install the components of a bundle from a file descriptor. See mf_client_install_file_async().
 * This blocks until the installation is done, or failed.
 *
 * Returns: (transfer full): what was installed, or %NULL on failure
 */
MfResult *
mf_client_install_file(MfClient *client,
                       int fd,
                       MfProgressFunc progress_func,
                       gpointer progress_data,
                       GCancellable *cancellable,
                       GError **error)
{
    g_auto(MfSyncCall) sync = {};

    mf_sync_call_begin(&sync);
    mf_client_install_file_async(client,
                                 fd,
                                 progress_func,
                                 progress_data,
                                 cancellable,
                                 mf_on_sync_call_done,
                                 &sync);
    return mf_client_install_file_finish(client, mf_sync_call_wait(&sync), error);
}

/**
 * mf_client_remove_component_async:
 * @client: an #MfClient
 * @component_id: identifier of the component to remove
 * @version: (nullable): the version to remove, or %NULL for all versions
 * @progress_func: (nullable) (scope notified): called whenever the service reports progress
 * @progress_data: passed to @progress_func
 * @cancellable: (nullable): a #GCancellable
 * @callback: called when the removal is done, or failed
 * @user_data: passed to @callback
 *
 * Remove a component and clean up any cruft it leaves behind.
 */
void
mf_client_remove_component_async(MfClient *client,
                                 const char *component_id,
                                 const char *version,
                                 MfProgressFunc progress_func,
                                 gpointer progress_data,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data)
{
    sd_json_variant *parameters = NULL;

    g_return_if_fail(MF_IS_CLIENT(client));
    g_return_if_fail(component_id != NULL);

    int r = sd_json_buildo(
        &parameters,
        SD_JSON_BUILD_PAIR_STRING("componentId", component_id),
        SD_JSON_BUILD_PAIR_CONDITION(version != NULL, "version", SD_JSON_BUILD_STRING(version)));
    if (r < 0) {
        GError *error = NULL;

        mf_set_build_parameters_error(&error, r);
        mf_client_return_error_async(client,
                                     cancellable,
                                     callback,
                                     user_data,
                                     mf_client_remove_component_async,
                                     error);
        return;
    }

    mf_client_call_async(client,
                         MF_VARLINK_METHOD_REMOVE_COMPONENT,
                         parameters,
                         -1,
                         TRUE,
                         progress_func,
                         progress_data,
                         cancellable,
                         callback,
                         user_data,
                         mf_client_remove_component_async);
    sd_json_variant_unref(parameters);
}

/**
 * mf_client_remove_component_finish:
 * @client: an #MfClient
 * @result: the result passed to the callback
 * @error: a #GError
 *
 * Returns: (transfer full): what was removed, or %NULL on failure
 */
MfResult *
mf_client_remove_component_finish(MfClient *client, GAsyncResult *result, GError **error)
{
    return mf_client_result_finish(client, result, mf_client_remove_component_async, error);
}

/**
 * mf_client_remove_component:
 * @client: an #MfClient
 * @component_id: identifier of the component to remove
 * @version: (nullable): the version to remove, or %NULL for all versions
 * @progress_func: (nullable) (scope call): called whenever the service reports progress
 * @progress_data: passed to @progress_func
 * @cancellable: (nullable): a #GCancellable
 * @error: a #GError
 *
 * Remove a component, see mf_client_remove_component_async(). This blocks
 * until the removal is done, or failed.
 *
 * Returns: (transfer full): what was removed, or %NULL on failure
 */
MfResult *
mf_client_remove_component(MfClient *client,
                           const char *component_id,
                           const char *version,
                           MfProgressFunc progress_func,
                           gpointer progress_data,
                           GCancellable *cancellable,
                           GError **error)
{
    g_auto(MfSyncCall) sync = {};

    mf_sync_call_begin(&sync);
    mf_client_remove_component_async(client,
                                     component_id,
                                     version,
                                     progress_func,
                                     progress_data,
                                     cancellable,
                                     mf_on_sync_call_done,
                                     &sync);
    return mf_client_remove_component_finish(client, mf_sync_call_wait(&sync), error);
}

/**
 * mf_client_cleanup_async:
 * @client: an #MfClient
 * @progress_func: (nullable) (scope notified): called whenever the service reports progress
 * @progress_data: passed to @progress_func
 * @cancellable: (nullable): a #GCancellable
 * @callback: called when the cleanup is done, or failed
 * @user_data: passed to @callback
 *
 * Remove obsolete data: Dependencies no installed component builds on,
 * unused component versions, and any cruft from interrupted installations or updates.
 */
void
mf_client_cleanup_async(MfClient *client,
                        MfProgressFunc progress_func,
                        gpointer progress_data,
                        GCancellable *cancellable,
                        GAsyncReadyCallback callback,
                        gpointer user_data)
{
    g_return_if_fail(MF_IS_CLIENT(client));

    mf_client_call_async(client,
                         MF_VARLINK_METHOD_CLEANUP,
                         NULL,
                         -1,
                         TRUE,
                         progress_func,
                         progress_data,
                         cancellable,
                         callback,
                         user_data,
                         mf_client_cleanup_async);
}

/**
 * mf_client_cleanup_finish:
 * @client: an #MfClient
 * @result: the result passed to the callback
 * @error: a #GError
 *
 * Returns: (transfer full): what was removed, or %NULL on failure
 */
MfResult *
mf_client_cleanup_finish(MfClient *client, GAsyncResult *result, GError **error)
{
    return mf_client_result_finish(client, result, mf_client_cleanup_async, error);
}

/**
 * mf_client_cleanup:
 * @client: an #MfClient
 * @progress_func: (nullable) (scope call): called whenever the service reports progress
 * @progress_data: passed to @progress_func
 * @cancellable: (nullable): a #GCancellable
 * @error: a #GError
 *
 * Remove obsolete data, see mf_client_cleanup_async().
 * This blocks until the cleanup is done, or failed.
 *
 * Returns: (transfer full): what was removed, or %NULL on failure
 */
MfResult *
mf_client_cleanup(MfClient *client,
                  MfProgressFunc progress_func,
                  gpointer progress_data,
                  GCancellable *cancellable,
                  GError **error)
{
    g_auto(MfSyncCall) sync = {};

    mf_sync_call_begin(&sync);
    mf_client_cleanup_async(client, progress_func, progress_data, cancellable, mf_on_sync_call_done, &sync);
    return mf_client_cleanup_finish(client, mf_sync_call_wait(&sync), error);
}

/**
 * mf_client_list_components_async:
 * @client: an #MfClient
 * @component_id: (nullable): only list the versions of this component
 * @kind: only list components of this kind, or %MF_COMPONENT_KIND_UNKNOWN for all
 * @cancellable: (nullable): a #GCancellable
 * @callback: called when the list arrived, or the call failed
 * @user_data: passed to @callback
 *
 * List installed components (apps/kits/platforms/cores) and their versions
 * and revisions. Can be filtered by kind.
 */
void
mf_client_list_components_async(MfClient *client,
                                const char *component_id,
                                MfComponentKind kind,
                                GCancellable *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer user_data)
{
    sd_json_variant *parameters = NULL;

    g_return_if_fail(MF_IS_CLIENT(client));

    int r = sd_json_buildo(
        &parameters,
        SD_JSON_BUILD_PAIR_CONDITION(component_id != NULL, "componentId", SD_JSON_BUILD_STRING(component_id)),
        SD_JSON_BUILD_PAIR_CONDITION(kind != MF_COMPONENT_KIND_UNKNOWN,
                                     "kind",
                                     SD_JSON_BUILD_STRING(mf_component_kind_to_string(kind))));
    if (r < 0) {
        GError *error = NULL;

        mf_set_build_parameters_error(&error, r);
        mf_client_return_error_async(client,
                                     cancellable,
                                     callback,
                                     user_data,
                                     mf_client_list_components_async,
                                     error);
        return;
    }

    mf_client_call_async(client,
                         MF_VARLINK_METHOD_LIST_COMPONENTS,
                         parameters,
                         -1,
                         FALSE,
                         NULL,
                         NULL,
                         cancellable,
                         callback,
                         user_data,
                         mf_client_list_components_async);
    sd_json_variant_unref(parameters);
}

/**
 * mf_client_list_components_finish:
 * @client: an #MfClient
 * @result: the result passed to the callback
 * @error: a #GError
 *
 * Returns: (transfer full) (element-type MfComponent): the components, or %NULL on error
 */
GPtrArray *
mf_client_list_components_finish(MfClient *client, GAsyncResult *result, GError **error)
{
    sd_json_variant *reply = mf_client_call_finish(client, result, mf_client_list_components_async, error);
    GPtrArray *components;

    if (reply == NULL)
        return NULL;
    components = mf_component_array_from_reply(sd_json_variant_by_key(reply, "components"), error);
    sd_json_variant_unref(reply);
    return components;
}

/**
 * mf_client_list_components:
 * @client: an #MfClient
 * @component_id: (nullable): only list the versions of this component
 * @kind: only list components of this kind, or %MF_COMPONENT_KIND_UNKNOWN for all
 * @cancellable: (nullable): a #GCancellable
 * @error: a #GError
 *
 * List what is installed, see mf_client_list_components_async(). This
 * blocks until the list arrived.
 *
 * Returns: (transfer full) (element-type MfComponent): the components, or %NULL on error
 */
GPtrArray *
mf_client_list_components(MfClient *client,
                          const char *component_id,
                          MfComponentKind kind,
                          GCancellable *cancellable,
                          GError **error)
{
    g_auto(MfSyncCall) sync = {};

    mf_sync_call_begin(&sync);
    mf_client_list_components_async(client, component_id, kind, cancellable, mf_on_sync_call_done, &sync);
    return mf_client_list_components_finish(client, mf_sync_call_wait(&sync), error);
}
