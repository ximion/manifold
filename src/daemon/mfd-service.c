/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mfd-service.h"

#include <errno.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#include <systemd/sd-json.h>
#include <systemd/sd-varlink.h>

#include <manifold.h>
#include "mf-interface-private.h"
#include "mf-sdautoptr-private.h"

#include "mfd-manager.h"
#include "mfd-policy.h"
#include "mf-varlink-util.h"

/* how long we stay around without a connection, when activated on demand */
constexpr guint64 IDLE_TIMEOUT_USEC = 30 * G_USEC_PER_SEC;

/**
 * MfdService:
 *
 * The service that manages the Manifold software installed on the host.
 * Every user can read the store directly, so for now the service only
 * handles requests that change it.
 *
 * The service speaks Varlink. A request that changes the installation needs
 * the authorization of polkit, see mfd_service_authorize(). These requests
 * run one at a time, in the order in which their callers were authorized.
 */
struct _MfdService {
    GObject parent_instance;

    MfdManager *manager;
    sd_varlink_server *server;
    sd_event_source *idle_timer; /* NULL unless we exit when idle */

    sd_bus *bus;                   /* the system bus, to ask polkit; NULL until we need it */
    GHashTable *requests;          /* the request of each connection that made one, see MfdRequest */
    GQueue queue;                  /* the authorized requests, in the order in which they will run */
    sd_event_source *queue_runner; /* runs the request at the head of the queue */
};

G_DEFINE_TYPE(MfdService, mfd_service, G_TYPE_OBJECT)

typedef struct _MfdRequest MfdRequest;

/**
 * MfdRequestFunc:
 *
 * Do what a request asks for, and reply to its call. The reply is an error
 * if the request failed.
 *
 * Returns: 0 if we replied, or a negative errno if we were unable to reply.
 */
typedef int (*MfdRequestFunc)(MfdService *service, MfdRequest *request);

/**
 * MfdRequest:
 *
 * A method call that changes the installation. We reply to it later: first
 * polkit has to authorize the caller, then the request waits in the queue
 * until the requests before it are done.
 *
 * sd-varlink handles one call of a connection at a time, so a connection
 * has at most one request.
 */
struct _MfdRequest {
    MfdService *service;
    sd_varlink *link;
    MfdRequestFunc run;
    const char *method;       /* the method that was called, for the log */
    const char *action_id;    /* the polkit action the caller needs */
    uid_t uid;                /* the calling user */
    gboolean report_progress; /* the call was made with the 'more' flag */
    sd_bus_slot *polkit_slot; /* the authorization check in progress, or NULL */

    int bundle_fd;      /* the bundle to install, or -1 */
    char *component_id; /* the component to remove or mark */
    char *version;      /* the version to remove or mark, or NULL for all of them */
    MfInstallMode mode; /* how to mark the component */
};

/**
 * mfd_request_new:
 * @link: The connection the method was called on.
 * @run: The function that does what the request asks for.
 * @method: The method that was called.
 * @action_id: The polkit action the caller needs.
 * @flags: The flags of the method call.
 *
 * Create a request for a method call. The caller fills in what the request
 * is about, and then passes it to mfd_service_authorize().
 */
static MfdRequest *
mfd_request_new(sd_varlink *link,
                MfdRequestFunc run,
                const char *method,
                const char *action_id,
                sd_varlink_method_flags_t flags)
{
    MfdRequest *request = g_new0(MfdRequest, 1);

    request->link = sd_varlink_ref(link);
    request->run = run;
    request->method = method;
    request->action_id = action_id;
    request->report_progress = (flags & SD_VARLINK_METHOD_MORE) != 0;
    request->bundle_fd = -1;

    return request;
}

static void
mfd_request_free(MfdRequest *request)
{
    sd_bus_slot_unref(request->polkit_slot);
    sd_varlink_unref(request->link);
    if (request->bundle_fd >= 0)
        close(request->bundle_fd);
    g_free(request->component_id);
    g_free(request->version);
    g_free(request);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MfdRequest, mfd_request_free)

static void
mfd_service_finalize(GObject *object)
{
    MfdService *service = MFD_SERVICE(object);

    sd_event_source_unref(service->queue_runner);
    g_queue_clear(&service->queue);
    g_hash_table_unref(service->requests);
    sd_bus_flush_close_unref(service->bus);
    sd_event_source_unref(service->idle_timer);
    sd_varlink_server_unref(service->server);
    g_clear_object(&service->manager);

    G_OBJECT_CLASS(mfd_service_parent_class)->finalize(object);
}

static void
mfd_service_class_init(MfdServiceClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = mfd_service_finalize;
}

static void
mfd_service_init(MfdService *service)
{
    service->requests = g_hash_table_new_full(g_direct_hash,
                                              g_direct_equal,
                                              NULL,
                                              (GDestroyNotify) mfd_request_free);
    g_queue_init(&service->queue);
}

/**
 * mfd_json_dispatch_transaction_flags:
 *
 * Read the "flags" parameter of a call into #MfTransactionFlags.
 * Flags we do not know are rejected, as we can not honor them.
 */
static int
mfd_json_dispatch_transaction_flags(const char *name,
                                    sd_json_variant *variant,
                                    sd_json_dispatch_flags_t flags,
                                    void *userdata)
{
    MfTransactionFlags *ret = userdata;

    *ret = MF_TRANSACTION_FLAG_NONE;
    if (sd_json_variant_is_null(variant))
        return 0;
    if (!sd_json_variant_is_array(variant))
        return -EINVAL;

    for (size_t i = 0; i < sd_json_variant_elements(variant); i++) {
        sd_json_variant *flag = sd_json_variant_by_index(variant, i);

        if (!sd_json_variant_is_string(flag))
            return -EINVAL;
        if (g_str_equal(sd_json_variant_string(flag), MF_VARLINK_TRANSACTION_FLAG_ALLOW_INTERACTIVE))
            mf_flags_add(*ret, MF_TRANSACTION_FLAG_INTERACTIVE);
        else
            return -EINVAL;
    }

    return 0;
}

/**
 * mfd_error_is_about_request:
 *
 * Returns: whether @error is about what was requested, rather than a
 *   failure of our own
 */
static gboolean
mfd_error_is_about_request(const GError *error)
{
    if (error->domain != MF_ERROR)
        return FALSE;

    switch (error->code) {
    case MF_ERROR_NOT_FOUND:
    case MF_ERROR_PERMISSION_DENIED:
    case MF_ERROR_INVALID_BUNDLE:
    case MF_ERROR_WRONG_ARCHITECTURE:
    case MF_ERROR_VERIFICATION_FAILED:
    case MF_ERROR_MISSING_BASE:
    case MF_ERROR_ALREADY_INSTALLED:
    case MF_ERROR_COMPONENT_IN_USE:
    case MF_ERROR_INVALID_MANIFEST:
    case MF_ERROR_REVOKED:
    case MF_ERROR_ID_CONFLICT:
        return TRUE;
    default:
        return FALSE;
    }
}

/**
 * mfd_service_reply_error:
 * @detail: (nullable): what the manager told about the error
 *
 * Reply with the error of the interface that corresponds to a #GError.
 *
 * Returns: The result of sd_varlink_error(), which is a negative errno
 *   even if the reply was sent.
 */
static int
mfd_service_reply_error(sd_varlink *link, const GError *error, const MfdErrorDetail *detail)
{
    const char *error_id = MF_VARLINK_ERROR_FAILED;

    /* we only warn about failures of our own, not about requests we can not fulfill */
    if (mfd_error_is_about_request(error))
        g_message("%s", error->message);
    else
        g_warning("%s", error->message);

    if (error->domain == MF_ERROR && detail != NULL && detail->component_id != NULL) {
        switch (error->code) {
        case MF_ERROR_MISSING_BASE:
            return sd_varlink_errorbo(link,
                                      MF_VARLINK_ERROR_MISSING_BASE,
                                      SD_JSON_BUILD_PAIR_STRING("componentId", detail->component_id),
                                      SD_JSON_BUILD_PAIR_STRING("baseId", detail->base_id),
                                      SD_JSON_BUILD_PAIR_STRING("baseVersion", detail->base_version),
                                      SD_JSON_BUILD_PAIR_STRING("identity", detail->identity));
        case MF_ERROR_ALREADY_INSTALLED:
            return sd_varlink_errorbo(link,
                                      MF_VARLINK_ERROR_ALREADY_INSTALLED,
                                      SD_JSON_BUILD_PAIR_STRING("componentId", detail->component_id),
                                      SD_JSON_BUILD_PAIR_STRING("version", detail->version));
        case MF_ERROR_COMPONENT_IN_USE:
            return sd_varlink_errorbo(link,
                                      MF_VARLINK_ERROR_COMPONENT_IN_USE,
                                      SD_JSON_BUILD_PAIR_STRING("componentId", detail->component_id),
                                      SD_JSON_BUILD_PAIR_STRING("usedBy", detail->used_by));
        case MF_ERROR_NOT_FOUND:
            return sd_varlink_errorbo(link,
                                      MF_VARLINK_ERROR_NO_SUCH_COMPONENT,
                                      SD_JSON_BUILD_PAIR_STRING("componentId", detail->component_id),
                                      SD_JSON_BUILD_PAIR_CONDITION(detail->version != NULL,
                                                                   "version",
                                                                   SD_JSON_BUILD_STRING(detail->version)));
        case MF_ERROR_WRONG_ARCHITECTURE:
            return sd_varlink_errorbo(
                link,
                MF_VARLINK_ERROR_WRONG_ARCHITECTURE,
                SD_JSON_BUILD_PAIR_STRING("componentId", detail->component_id),
                SD_JSON_BUILD_PAIR_STRING("architecture", detail->architecture),
                SD_JSON_BUILD_PAIR_STRING("hostArchitecture", mf_get_host_architecture()));
        case MF_ERROR_ID_CONFLICT:
            return sd_varlink_errorbo(
                link,
                MF_VARLINK_ERROR_ID_CONFLICT,
                SD_JSON_BUILD_PAIR_STRING("componentId", detail->component_id),
                SD_JSON_BUILD_PAIR_STRING("kind", mf_component_kind_to_string(detail->kind)),
                SD_JSON_BUILD_PAIR_BOOLEAN("sdk", detail->sdk));
        default:
            break;
        }
    }

    if (error->domain == MF_ERROR) {
        switch (error->code) {
        case MF_ERROR_INVALID_BUNDLE:
        case MF_ERROR_INVALID_MANIFEST:
            /* a manifest is only ever installed as part of a bundle */
            error_id = MF_VARLINK_ERROR_INVALID_BUNDLE;
            break;
        case MF_ERROR_VERIFICATION_FAILED:
            error_id = MF_VARLINK_ERROR_VERIFICATION_FAILED;
            break;
        default:
            break;
        }
    }

    return sd_varlink_errorbo(link, error_id, SD_JSON_BUILD_PAIR_STRING("message", error->message));
}

/**
 * mfd_varlink_append_component:
 *
 * Add an installed component to the array of a reply.
 */
static int
mfd_varlink_append_component(sd_json_variant **array, MfComponent *component)
{
    return sd_json_variant_append_arraybo(
        array,
        SD_JSON_BUILD_PAIR_STRING("componentId", mf_component_get_id(component)),
        SD_JSON_BUILD_PAIR_STRING("kind", mf_component_kind_to_string(mf_component_get_kind(component))),
        SD_JSON_BUILD_PAIR_STRING("version", mf_component_get_version(component)),
        SD_JSON_BUILD_PAIR_UNSIGNED("revision", mf_component_get_revision(component)),
        SD_JSON_BUILD_PAIR_BOOLEAN("sdk", mf_component_is_sdk(component)),
        SD_JSON_BUILD_PAIR_STRING("identity", mf_component_get_identity(component)),
        SD_JSON_BUILD_PAIR_STRING("verityHash", mf_component_get_verity_hash(component)),
        SD_JSON_BUILD_PAIR_BOOLEAN("current", mf_component_is_current(component)),
        SD_JSON_BUILD_PAIR_STRING("installMode",
                                  mf_install_mode_to_string(mf_component_get_install_mode(component))),
        SD_JSON_BUILD_PAIR_STRING("origin", mf_component_get_origin(component)));
}

/**
 * mfd_json_ensure_array:
 *
 * Appending to an array that has no elements leaves it %NULL. An empty
 * array is still an array in a reply, so make one if nothing was appended.
 *
 * Returns: 0, or a negative errno
 */
static int
mfd_json_ensure_array(sd_json_variant **array)
{
    if (*array != NULL)
        return 0;
    return sd_json_variant_new_array(array, NULL, 0);
}

/**
 * mfd_varlink_build_components_array:
 *
 * Build the array of components of a reply.
 *
 * Returns: 0, or a negative errno
 */
static int
mfd_varlink_build_components_array(GPtrArray *components, sd_json_variant **ret_array)
{
    g_autoptr(sd_json_variant) array = NULL;
    int r;

    for (guint i = 0; i < components->len; i++) {
        if ((r = mfd_varlink_append_component(&array, g_ptr_array_index(components, i))) < 0)
            return r;
    }
    if ((r = mfd_json_ensure_array(&array)) < 0)
        return r;

    *ret_array = g_steal_pointer(&array);
    return 0;
}

/**
 * mfd_service_reply_result:
 *
 * Send the final reply of an operation: the components it installed and
 * removed, the images it removed, and the disk space it used and freed.
 */
static int
mfd_service_reply_result(sd_varlink *link, MfResult *result)
{
    g_autoptr(sd_json_variant) installed = NULL;
    g_autoptr(sd_json_variant) removed = NULL;
    g_autoptr(sd_json_variant) removed_images = NULL;
    GPtrArray *images = mf_result_get_removed_images(result);
    int r;

    if ((r = mfd_varlink_build_components_array(mf_result_get_installed(result), &installed)) < 0)
        return r;
    if ((r = mfd_varlink_build_components_array(mf_result_get_removed(result), &removed)) < 0)
        return r;
    for (guint i = 0; i < images->len; i++) {
        r = sd_json_variant_append_arrayb(&removed_images,
                                          SD_JSON_BUILD_STRING(g_ptr_array_index(images, i)));
        if (r < 0)
            return r;
    }
    if ((r = mfd_json_ensure_array(&removed_images)) < 0)
        return r;

    return sd_varlink_replybo(
        link,
        SD_JSON_BUILD_PAIR(
            "result",
            SD_JSON_BUILD_OBJECT(
                SD_JSON_BUILD_PAIR_VARIANT("installed", installed),
                SD_JSON_BUILD_PAIR_VARIANT("removed", removed),
                SD_JSON_BUILD_PAIR_VARIANT("removedImages", removed_images),
                SD_JSON_BUILD_PAIR_UNSIGNED("bytesAdded", mf_result_get_bytes_added(result)),
                SD_JSON_BUILD_PAIR_UNSIGNED("bytesFreed", mf_result_get_bytes_freed(result)))));
}

/**
 * mfd_varlink_can_write:
 *
 * Check whether the socket of a connection can take more data right now.
 */
static gboolean
mfd_varlink_can_write(sd_varlink *link)
{
    struct pollfd pfd = {.fd = sd_varlink_get_fd(link), .events = POLLOUT};

    return pfd.fd >= 0 && poll(&pfd, 1, 0) > 0 && (pfd.revents & (POLLOUT | POLLERR | POLLHUP)) == POLLOUT;
}

/**
 * mfd_service_on_progress:
 *
 * Send the state of the operation to the caller.
 */
static void
mfd_service_on_progress(const MfProgress *progress, gpointer user_data)
{
    sd_varlink *link = user_data;
    const MfItemProgress *item = mf_progress_get_item(progress);
    const char *action = item != NULL ? mf_action_to_string(mf_item_progress_get_action(item)) : NULL;
    const char *subject = item != NULL ? mf_item_progress_get_subject(item) : NULL;
    const char *version = item != NULL ? mf_item_progress_get_version(item) : NULL;
    double item_percent = item != NULL ? mf_item_progress_get_percent(item) : 0;
    guint64 bytes_done = item != NULL ? mf_item_progress_get_bytes_done(item) : 0;
    guint64 bytes_total = item != NULL ? mf_item_progress_get_bytes_total(item) : 0;

    /* NOTE: The operation runs in the event loop, so nothing else is handled until it is done.
     * We therefore never wait for a caller that does not read its progress, and drop the
     * progress while its socket is full. A socket that can take more data has most of its
     * buffer free, so a progress message is written out at once below. */
    if (!mfd_varlink_can_write(link))
        return;

    int r = sd_varlink_notifybo(
        link,
        SD_JSON_BUILD_PAIR("progress",
                           SD_JSON_BUILD_OBJECT(
                               SD_JSON_BUILD_PAIR_REAL("percent", mf_progress_get_percent(progress)),
                               SD_JSON_BUILD_PAIR_CONDITION(
                                   item != NULL,
                                   "item",
                                   SD_JSON_BUILD_OBJECT(
                                       SD_JSON_BUILD_PAIR_STRING("action", action),
                                       SD_JSON_BUILD_PAIR_STRING("subject", subject),
                                       SD_JSON_BUILD_PAIR_CONDITION(version != NULL,
                                                                    "version",
                                                                    SD_JSON_BUILD_STRING(version)),
                                       SD_JSON_BUILD_PAIR_REAL("percent", item_percent),
                                       SD_JSON_BUILD_PAIR_CONDITION(bytes_total > 0,
                                                                    "bytesDone",
                                                                    SD_JSON_BUILD_UNSIGNED(bytes_done)),
                                       SD_JSON_BUILD_PAIR_CONDITION(bytes_total > 0,
                                                                    "bytesTotal",
                                                                    SD_JSON_BUILD_UNSIGNED(bytes_total)))))));
    /* sd-varlink only queues the notification, and we are busy, so we write it out now */
    if (r >= 0)
        r = sd_varlink_flush(link);
    if (r < 0)
        g_warning("Unable to report progress: %s", g_strerror(-r));
}

/**
 * mfd_service_install_bundle:
 *
 * Install everything a bundle contains, and reply to the call.
 */
static int
mfd_service_install_bundle(MfdService *service, MfdRequest *request)
{
    g_auto(MfdErrorDetail) detail = {};
    g_autoptr(MfBundle) bundle = NULL;
    g_autoptr(MfResult) result = NULL;
    g_autoptr(GError) error = NULL;

    g_message("User %u requested to install a bundle", (unsigned int) request->uid);

    bundle = mf_bundle_new_from_fd(request->bundle_fd, &error);
    if (bundle == NULL) {
        mfd_service_reply_error(request->link, error, NULL);
        return 0;
    }

    /* TODO: Verify the signature against trusted keys, once a host has some. */
    if (mf_bundle_is_signed(bundle))
        g_warning(
            "The bundle is signed, but the signature was not verified: trusted keys are not supported yet");
    else
        g_warning("The bundle is not signed");

    result = mfd_manager_install_bundle(service->manager,
                                        bundle,
                                        request->report_progress ? mfd_service_on_progress : NULL,
                                        request->link,
                                        &detail,
                                        &error);
    if (result == NULL) {
        mfd_service_reply_error(request->link, error, &detail);
        return 0;
    }

    return mfd_service_reply_result(request->link, result);
}

/**
 * mfd_service_remove:
 *
 * Remove the component of the request, or clean up if the request names
 * no component, and reply to the call.
 */
static int
mfd_service_remove(MfdService *service, MfdRequest *request)
{
    g_auto(MfdErrorDetail) detail = {};
    g_autoptr(MfResult) result = NULL;
    g_autoptr(GError) error = NULL;

    if (request->component_id == NULL)
        g_message("User %u requested to clean up", (unsigned int) request->uid);
    else
        g_message("User %u requested to remove %s %s",
                  (unsigned int) request->uid,
                  request->component_id,
                  request->version != NULL ? request->version : "");

    result = mfd_manager_remove(service->manager,
                                request->component_id,
                                request->version,
                                request->report_progress ? mfd_service_on_progress : NULL,
                                request->link,
                                &detail,
                                &error);
    if (result == NULL) {
        mfd_service_reply_error(request->link, error, &detail);
        return 0;
    }

    return mfd_service_reply_result(request->link, result);
}

/**
 * mfd_service_mark:
 *
 * Mark the component of the request, and reply to the call.
 */
static int
mfd_service_mark(MfdService *service, MfdRequest *request)
{
    g_auto(MfdErrorDetail) detail = {};
    g_autoptr(GPtrArray) marked = NULL;
    g_autoptr(sd_json_variant) components = NULL;
    g_autoptr(GError) error = NULL;
    int r;

    g_message("User %u requested to mark %s %s as %s",
              (unsigned int) request->uid,
              request->component_id,
              request->version != NULL ? request->version : "",
              mf_install_mode_to_string(request->mode));

    marked = mfd_manager_mark(service->manager,
                              request->component_id,
                              request->version,
                              request->mode,
                              &detail,
                              &error);
    if (marked == NULL) {
        mfd_service_reply_error(request->link, error, &detail);
        return 0;
    }
    if ((r = mfd_varlink_build_components_array(marked, &components)) < 0)
        return r;

    return sd_varlink_replybo(
        request->link,
        SD_JSON_BUILD_PAIR("done",
                           SD_JSON_BUILD_OBJECT(SD_JSON_BUILD_PAIR_VARIANT("components", components))));
}

/**
 * mfd_service_drop_request:
 *
 * Forget a request, whether it is waiting for polkit or in the queue.
 */
static void
mfd_service_drop_request(MfdService *service, MfdRequest *request)
{
    g_queue_remove(&service->queue, request);
    g_hash_table_remove(service->requests, request->link);
}

/**
 * mfd_service_queue_request:
 *
 * Add an authorized request to the end of the queue.
 */
static void
mfd_service_queue_request(MfdService *service, MfdRequest *request)
{
    g_queue_push_tail(&service->queue, request);
    sd_event_source_set_enabled(service->queue_runner, SD_EVENT_ON);
}

/**
 * mfd_service_on_run_queue:
 *
 * Run the request at the head of the queue. We run one request per iteration
 * of the event loop, so new calls and the answers of polkit are handled
 * between two requests.
 */
static int
mfd_service_on_run_queue(sd_event_source *source, void *userdata)
{
    MfdService *service = userdata;
    MfdRequest *request = g_queue_pop_head(&service->queue);
    int r;

    if (g_queue_is_empty(&service->queue))
        sd_event_source_set_enabled(source, SD_EVENT_OFF);
    if (request == NULL)
        return 0;

    r = request->run(service, request);
    if (r < 0) {
        g_warning("Unable to reply to the call of %s by user %u: %s",
                  request->method,
                  (unsigned int) request->uid,
                  g_strerror(-r));
        /* if building the reply failed, this at least tells the caller that the call failed */
        sd_varlink_error_errno(request->link, r);
    }
    g_hash_table_remove(service->requests, request->link);

    return 0;
}

/**
 * mfd_service_refuse_request:
 * @error: (nullable): Why polkit could not be asked, or %NULL if polkit denied the request.
 *
 * Reply to a request that may not run, and forget it.
 *
 * Returns: The result of sd_varlink_error(), which is a negative errno
 *   even if the reply was sent.
 */
static int
mfd_service_refuse_request(MfdService *service, MfdRequest *request, const GError *error)
{
    int r;

    if (error != NULL) {
        g_warning("Unable to authorize user %u for %s: %s",
                  (unsigned int) request->uid,
                  request->method,
                  error->message);
        r = sd_varlink_errorbo(request->link,
                               MF_VARLINK_ERROR_FAILED,
                               SD_JSON_BUILD_PAIR_STRING("message", error->message));
    } else {
        g_message("Denied call of %s by user %u: not authorized for %s",
                  request->method,
                  (unsigned int) request->uid,
                  request->action_id);
        r = sd_varlink_error(request->link, SD_VARLINK_ERROR_PERMISSION_DENIED, NULL);
    }
    mfd_service_drop_request(service, request);

    return r;
}

/**
 * mfd_service_on_polkit_reply:
 *
 * Queue a request once polkit has authorized its caller, or refuse it.
 */
static int
mfd_service_on_polkit_reply(sd_bus_message *reply, void *userdata, sd_bus_error *ret_error)
{
    MfdRequest *request = userdata;
    MfdService *service = request->service;
    g_autoptr(GError) error = NULL;
    int r;

    request->polkit_slot = sd_bus_slot_unref(request->polkit_slot);

    r = mfd_policy_read_reply(reply, &error);
    if (r > 0) {
        g_message("User %u is authorized for %s", (unsigned int) request->uid, request->action_id);
        mfd_service_queue_request(service, request);
        return 0;
    }

    mfd_service_refuse_request(service, request, error);
    return 0;
}

/**
 * mfd_service_get_bus:
 *
 * Returns: (transfer none) (nullable): The system bus, which we connect to
 *   when we need it first, or %NULL on error.
 */
static sd_bus *
mfd_service_get_bus(MfdService *service, GError **error)
{
    /* if the bus was closed, e.g. because the bus daemon was restarted, we connect again */
    if (service->bus != NULL && sd_bus_is_open(service->bus) <= 0)
        service->bus = sd_bus_flush_close_unref(service->bus);
    if (service->bus == NULL)
        service->bus = mfd_policy_connect(sd_varlink_server_get_event(service->server), error);

    return service->bus;
}

/**
 * mfd_service_authorize:
 * @request: (transfer full): A request whose caller is not authorized yet.
 * @flags: The transaction flags of the call.
 *
 * Check whether the caller may make @request. Root and the user we are
 * running as may do everything, so we queue their requests right away.
 * For everyone else we ask polkit, and queue the request once polkit has
 * authorized the caller, see mfd_service_on_polkit_reply(). We do not wait
 * for polkit, as it may ask the user for a password, and the requests of
 * other callers must not wait for that.
 *
 * Returns: 0, or a negative errno to return from the method callback.
 */
static int
mfd_service_authorize(MfdService *service, MfdRequest *request, MfTransactionFlags flags)
{
    g_autoptr(GError) error = NULL;
    sd_bus *bus;
    int r;

    request->service = service;
    g_hash_table_insert(service->requests, request->link, request);

    r = mf_varlink_get_peer_uid(request->link, &request->uid);
    if (r < 0) {
        mfd_service_drop_request(service, request);
        return r;
    }

    if (mf_peer_is_privileged(request->uid)) {
        mfd_service_queue_request(service, request);
        return 0;
    }

    bus = mfd_service_get_bus(service, &error);
    if (bus == NULL || !mfd_policy_check_async(bus,
                                               request->link,
                                               request->uid,
                                               request->action_id,
                                               mf_flags_contains(flags, MF_TRANSACTION_FLAG_INTERACTIVE),
                                               mfd_service_on_polkit_reply,
                                               request,
                                               &request->polkit_slot,
                                               &error))
        return mfd_service_refuse_request(service, request, error);

    return 0;
}

/**
 * mfd_varlink_method_install_file:
 */
static int
mfd_varlink_method_install_file(sd_varlink *link,
                                sd_json_variant *parameters,
                                sd_varlink_method_flags_t flags,
                                void *userdata)
{
    MfdService *service = userdata;
    struct {
        unsigned int fd_index;
        MfTransactionFlags flags;
    } p = {};
    static const sd_json_dispatch_field dispatch_table[] = {
        {"bundleFileDescriptor",
         _SD_JSON_VARIANT_TYPE_INVALID, sd_json_dispatch_uint,
         offsetof(typeof(p), fd_index),
         SD_JSON_MANDATORY},
        {"flags",
         _SD_JSON_VARIANT_TYPE_INVALID, mfd_json_dispatch_transaction_flags,
         offsetof(typeof(p), flags),
         0},
        {},
    };
    g_autoptr(MfdRequest) request = NULL;
    struct stat st;
    int r;

    r = sd_varlink_dispatch(link, parameters, dispatch_table, &p);
    if (r != 0)
        return r;

    request = mfd_request_new(link,
                              mfd_service_install_bundle,
                              MF_VARLINK_METHOD_INSTALL_FILE,
                              MFD_POLKIT_ACTION_INSTALL_UNTRUSTED,
                              flags);

    /* a malformed call is refused before polkit may ask the user for a password */
    request->bundle_fd = sd_varlink_take_fd(link, p.fd_index);
    if (request->bundle_fd < 0)
        return sd_varlink_error_invalid_parameter_name(link, "bundleFileDescriptor");
    if (fstat(request->bundle_fd, &st) < 0 || !S_ISREG(st.st_mode))
        return sd_varlink_errorbo(link,
                                  MF_VARLINK_ERROR_INVALID_BUNDLE,
                                  SD_JSON_BUILD_PAIR_STRING("message", "Not a regular file"));

    /*
     * We only read the bundle once the caller is authorized to install it,
     * so an unauthorized caller can not make us do that work as root.
     * TODO: Once there is a store of trusted keys, verify the signature of the
     * index before we ask polkit (reading only the index and its signature),
     * so that a trusted bundle only needs the less strict action.
     */
    return mfd_service_authorize(service, g_steal_pointer(&request), p.flags);
}

/**
 * mfd_varlink_method_remove_component:
 */
static int
mfd_varlink_method_remove_component(sd_varlink *link,
                                    sd_json_variant *parameters,
                                    sd_varlink_method_flags_t flags,
                                    void *userdata)
{
    MfdService *service = userdata;
    struct {
        const char *component_id;
        const char *version;
        MfTransactionFlags flags;
    } p = {};
    static const sd_json_dispatch_field dispatch_table[] = {
        {"componentId",
         SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string,
         offsetof(typeof(p), component_id),
         SD_JSON_MANDATORY},
        {"version", SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string, offsetof(typeof(p), version), 0},
        {"flags",
         _SD_JSON_VARIANT_TYPE_INVALID, mfd_json_dispatch_transaction_flags,
         offsetof(typeof(p), flags),
         0},
        {},
    };
    MfdRequest *request;
    int r;

    r = sd_varlink_dispatch(link, parameters, dispatch_table, &p);
    if (r != 0)
        return r;

    /* a malformed call is refused before polkit may ask the user for a password */
    if (!mf_component_id_is_valid(p.component_id))
        return sd_varlink_error_invalid_parameter_name(link, "componentId");
    if (p.version != NULL && !mf_version_is_valid(p.version))
        return sd_varlink_error_invalid_parameter_name(link, "version");

    request = mfd_request_new(link,
                              mfd_service_remove,
                              MF_VARLINK_METHOD_REMOVE_COMPONENT,
                              MFD_POLKIT_ACTION_REMOVE,
                              flags);
    request->component_id = g_strdup(p.component_id);
    request->version = g_strdup(p.version);

    return mfd_service_authorize(service, request, p.flags);
}

/**
 * mfd_varlink_method_cleanup:
 */
static int
mfd_varlink_method_cleanup(sd_varlink *link,
                           sd_json_variant *parameters,
                           sd_varlink_method_flags_t flags,
                           void *userdata)
{
    MfdService *service = userdata;
    static const sd_json_dispatch_field dispatch_table[] = {
        {"flags", _SD_JSON_VARIANT_TYPE_INVALID, mfd_json_dispatch_transaction_flags, 0, 0},
        {},
    };
    MfTransactionFlags transaction_flags = MF_TRANSACTION_FLAG_NONE;
    MfdRequest *request;
    int r;

    r = sd_varlink_dispatch(link, parameters, dispatch_table, &transaction_flags);
    if (r != 0)
        return r;

    /* a request without a component cleans up, see mfd_service_remove() */
    request = mfd_request_new(link,
                              mfd_service_remove,
                              MF_VARLINK_METHOD_CLEANUP,
                              MFD_POLKIT_ACTION_REMOVE,
                              flags);

    return mfd_service_authorize(service, request, transaction_flags);
}

/**
 * mfd_varlink_method_mark_component:
 */
static int
mfd_varlink_method_mark_component(sd_varlink *link,
                                  sd_json_variant *parameters,
                                  sd_varlink_method_flags_t flags,
                                  void *userdata)
{
    MfdService *service = userdata;
    struct {
        const char *component_id;
        const char *version;
        const char *mode;
        MfTransactionFlags flags;
    } p = {};
    static const sd_json_dispatch_field dispatch_table[] = {
        {"componentId",
         SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string,
         offsetof(typeof(p), component_id),
         SD_JSON_MANDATORY},
        {"version", SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string, offsetof(typeof(p), version), 0},
        {"mode",
         SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string,
         offsetof(typeof(p), mode),
         SD_JSON_MANDATORY},
        {"flags",
         _SD_JSON_VARIANT_TYPE_INVALID, mfd_json_dispatch_transaction_flags,
         offsetof(typeof(p), flags),
         0},
        {},
    };
    MfdRequest *request;
    MfInstallMode mode;
    int r;

    r = sd_varlink_dispatch(link, parameters, dispatch_table, &p);
    if (r != 0)
        return r;

    /* a malformed call is refused before polkit may ask the user for a password */
    if (!mf_component_id_is_valid(p.component_id))
        return sd_varlink_error_invalid_parameter_name(link, "componentId");
    if (p.version != NULL && !mf_version_is_valid(p.version))
        return sd_varlink_error_invalid_parameter_name(link, "version");
    mode = mf_install_mode_from_string(p.mode);
    if (mode == MF_INSTALL_MODE_UNKNOWN)
        return sd_varlink_error_invalid_parameter_name(link, "mode");

    request = mfd_request_new(link,
                              mfd_service_mark,
                              MF_VARLINK_METHOD_MARK_COMPONENT,
                              MFD_POLKIT_ACTION_MARK,
                              flags);
    request->component_id = g_strdup(p.component_id);
    request->version = g_strdup(p.version);
    request->mode = mode;

    return mfd_service_authorize(service, request, p.flags);
}

/**
 * mfd_service_new:
 * @event: The event loop to run the service in.
 * @error: A #GError
 *
 * Create the service. It does not listen for connections yet.
 */
MfdService *
mfd_service_new(sd_event *event, GError **error)
{
    g_autoptr(MfdService) service = NULL;
    int r;

    service = g_object_new(MFD_TYPE_SERVICE, NULL);
    service->manager = mfd_manager_new();

    service->server = mf_varlink_server_new(event,
                                            "manifoldd",
#if HAVE_SD_VARLINK_SERVER_FD_PASSING
                                            SD_VARLINK_SERVER_ALLOW_FD_PASSING_INPUT,
#else
                                            0, /* mfd_on_connect() allows it per connection */
#endif
                                            &vl_interface_org_freedesktop_manifold,
                                            service,
                                            error);
    if (service->server == NULL)
        return NULL;

    r = sd_varlink_server_bind_method_many(service->server,
                                           MF_VARLINK_METHOD_INSTALL_FILE,
                                           mfd_varlink_method_install_file,
                                           MF_VARLINK_METHOD_REMOVE_COMPONENT,
                                           mfd_varlink_method_remove_component,
                                           MF_VARLINK_METHOD_CLEANUP,
                                           mfd_varlink_method_cleanup,
                                           MF_VARLINK_METHOD_MARK_COMPONENT,
                                           mfd_varlink_method_mark_component);
    if (r < 0) {
        mf_set_error_from_errno(error, r, "Unable to bind Varlink methods");
        return NULL;
    }

    /* the queue runner is only enabled while there are requests in the queue */
    r = sd_event_add_defer(event, &service->queue_runner, mfd_service_on_run_queue, service);
    if (r >= 0)
        r = sd_event_source_set_enabled(service->queue_runner, SD_EVENT_OFF);
    if (r < 0) {
        mf_set_error_from_errno(error, r, "Unable to set up the request queue");
        return NULL;
    }

    return g_steal_pointer(&service);
}

/**
 * mfd_on_idle_timeout:
 */
static int
mfd_on_idle_timeout(sd_event_source *source, uint64_t usec, void *userdata)
{
    g_info("Nothing to do, exiting");
    return sd_event_exit(sd_event_source_get_event(source), 0);
}

/**
 * mfd_on_connect:
 */
static int
mfd_on_connect(sd_varlink_server *server, sd_varlink *link, void *userdata)
{
    MfdService *service = userdata;

#if !HAVE_SD_VARLINK_SERVER_FD_PASSING
    /* systemd < 258 can only allow receiving file descriptors per connection,
     * and we need them for the files clients ask us to install */
    int r = sd_varlink_set_allow_fd_passing_input(link, true);
    if (r < 0)
        return r;
#endif

    /* the idle timer only exists when we were socket activated */
    if (service->idle_timer != NULL)
        sd_event_source_set_enabled(service->idle_timer, SD_EVENT_OFF);
    return 0;
}

/**
 * mfd_on_disconnect:
 */
static void
mfd_on_disconnect(sd_varlink_server *server, sd_varlink *link, void *userdata)
{
    MfdService *service = userdata;
    MfdRequest *request = g_hash_table_lookup(service->requests, link);

    /* nobody waits for the reply to a request anymore once its caller is gone */
    if (request != NULL) {
        g_message("User %u disconnected before their call of %s was done",
                  (unsigned int) request->uid,
                  request->method);
        mfd_service_drop_request(service, request);
    }

    /* the idle timer only exists when we were socket activated */
    if (service->idle_timer == NULL)
        return;
    /* the closing connection is not included in the count anymore */
    if (sd_varlink_server_current_connections(server) > 0)
        return;
    sd_event_source_set_time_relative(service->idle_timer, IDLE_TIMEOUT_USEC);
    sd_event_source_set_enabled(service->idle_timer, SD_EVENT_ONESHOT);
}

/**
 * mfd_service_exit_when_idle:
 *
 * Exit once nobody has talked to us for a while. We do not use the
 * exit-on-idle of sd-varlink: it leaves the event loop the moment no
 * connection is open, which is right away when we were started with a
 * listening socket.
 */
static int
mfd_service_exit_when_idle(MfdService *service)
{
    /* the timer is stopped while a connection is open, see mfd_on_connect() and mfd_on_disconnect() */
    return sd_event_add_time_relative(sd_varlink_server_get_event(service->server),
                                      &service->idle_timer,
                                      CLOCK_MONOTONIC,
                                      IDLE_TIMEOUT_USEC,
                                      G_USEC_PER_SEC,
                                      mfd_on_idle_timeout,
                                      NULL);
}

/**
 * mfd_service_listen:
 * @service: The service.
 * @socket_path: (nullable): Path of the socket to listen on, or %NULL
 * @error: A #GError
 *
 * Listen for connections. If no socket path is given, we listen on the socket
 * we were activated with, and exit once we are idle.
 */
gboolean
mfd_service_listen(MfdService *service, const char *socket_path, GError **error)
{
    gboolean activated;
    int r;

    sd_varlink_server_bind_connect(service->server, mfd_on_connect);
    sd_varlink_server_bind_disconnect(service->server, mfd_on_disconnect);
    if (!mf_varlink_server_listen(service->server, socket_path, MF_SOCKET_PATH, &activated, error))
        return FALSE;
    if (!activated)
        return TRUE;

    r = mfd_service_exit_when_idle(service);
    if (r < 0) {
        mf_set_error_from_errno(error, r, "Unable to set up the idle timer");
        return FALSE;
    }

    return TRUE;
}
