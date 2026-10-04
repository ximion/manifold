/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mfd-service.h"

#include <errno.h>
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
 * the authorization of polkit.
 */
struct _MfdService {
    GObject parent_instance;

    MfdManager *manager;
    sd_varlink_server *server;
    sd_event_source *idle_timer; /* NULL unless we exit when idle */
};

G_DEFINE_TYPE(MfdService, mfd_service, G_TYPE_OBJECT)

static void
mfd_service_finalize(GObject *object)
{
    MfdService *service = MFD_SERVICE(object);

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
 * mfd_service_reply_error:
 * @detail: (nullable): what the manager told about the error
 *
 * Reply with the error of the interface that corresponds to a #GError.
 */
static int
mfd_service_reply_error(sd_varlink *link, const GError *error, const MfdErrorDetail *detail)
{
    const char *error_id = MF_VARLINK_ERROR_FAILED;

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
        default:
            break;
        }
    }

    if (error->domain == MF_ERROR) {
        switch (error->code) {
        case MF_ERROR_INVALID_BUNDLE:
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
    /* a notification is only queued - we are busy, so write it out now */
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
mfd_service_install_bundle(MfdService *service, sd_varlink *link, int fd, gboolean report_progress_wanted)
{
    g_auto(MfdErrorDetail) detail = {};
    g_autoptr(MfBundle) bundle = NULL;
    g_autoptr(MfResult) result = NULL;
    g_autoptr(GError) error = NULL;

    bundle = mf_bundle_new_from_fd(fd, &error);
    if (bundle == NULL)
        return mfd_service_reply_error(link, error, NULL);

    /* TODO: Verify the signature against trusted keys, once a host has some. */
    if (mf_bundle_is_signed(bundle))
        g_warning(
            "The bundle is signed, but the signature was not verified: trusted keys are not supported yet");
    else
        g_warning("The bundle is not signed");

    result = mfd_manager_install_bundle(service->manager,
                                        bundle,
                                        report_progress_wanted ? mfd_service_on_progress : NULL,
                                        link,
                                        &detail,
                                        &error);
    if (result == NULL)
        return mfd_service_reply_error(link, error, &detail);

    return mfd_service_reply_result(link, result);
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
    struct stat st;
    uid_t peer_uid;
    int fd;
    int r;

    /* the flags say whether polkit may ask, so they are needed first */
    r = sd_varlink_dispatch(link, parameters, dispatch_table, &p);
    if (r != 0)
        return r;

    /*
     * The bundle is opened only once the caller may install it, as reading it
     * as root is work an unauthorized caller should not be able to cause.
     * TODO: Once there is a store of trusted keys, verify the signature of the
     * index against it before this (reading only the index and its signature),
     * so that a trusted bundle needs only the less strict action.
     */
    r = mfd_policy_check_authorized(link,
                                    MF_VARLINK_METHOD_INSTALL_FILE,
                                    MFD_POLKIT_ACTION_INSTALL_UNTRUSTED,
                                    p.flags,
                                    &peer_uid);
    if (r != 0)
        return r;

    fd = sd_varlink_take_fd(link, p.fd_index);
    if (fd < 0)
        return sd_varlink_error_invalid_parameter_name(link, "bundleFileDescriptor");
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return sd_varlink_errorbo(link,
                                  MF_VARLINK_ERROR_INVALID_BUNDLE,
                                  SD_JSON_BUILD_PAIR_STRING("message", "Not a regular file"));
    }

    g_info("User %u requested to install a bundle", (unsigned int) peer_uid);
    r = mfd_service_install_bundle(service, link, fd, (flags & SD_VARLINK_METHOD_MORE) != 0);
    close(fd);

    return r;
}

/**
 * mfd_service_remove:
 * @component_id: (nullable): the component to remove, or %NULL to clean up
 * @version: (nullable): the version to remove, or %NULL for all versions
 *
 * Remove a component (and/or any remaining cruft) and reply to the call.
 */
static int
mfd_service_remove(MfdService *service,
                   sd_varlink *link,
                   const char *component_id,
                   const char *version,
                   gboolean report_progress_wanted)
{
    g_auto(MfdErrorDetail) detail = {};
    g_autoptr(MfResult) result = NULL;
    g_autoptr(GError) error = NULL;

    result = mfd_manager_remove(service->manager,
                                component_id,
                                version,
                                report_progress_wanted ? mfd_service_on_progress : NULL,
                                link,
                                &detail,
                                &error);
    if (result == NULL)
        return mfd_service_reply_error(link, error, &detail);

    return mfd_service_reply_result(link, result);
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
    uid_t peer_uid;
    int r;

    /* the flags say whether polkit may ask, so they are needed first */
    r = sd_varlink_dispatch(link, parameters, dispatch_table, &p);
    if (r != 0)
        return r;

    r = mfd_policy_check_authorized(link,
                                    MF_VARLINK_METHOD_REMOVE_COMPONENT,
                                    MFD_POLKIT_ACTION_REMOVE,
                                    p.flags,
                                    &peer_uid);
    if (r != 0)
        return r;
    if (!mf_component_id_is_valid(p.component_id))
        return sd_varlink_error_invalid_parameter_name(link, "componentId");
    if (p.version != NULL && !mf_version_is_valid(p.version))
        return sd_varlink_error_invalid_parameter_name(link, "version");

    g_info("User %u requested to remove %s %s",
           (unsigned int) peer_uid,
           p.component_id,
           p.version ? p.version : "");

    return mfd_service_remove(service,
                              link,
                              p.component_id,
                              p.version,
                              (flags & SD_VARLINK_METHOD_MORE) != 0);
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
    uid_t peer_uid;
    int r;

    /* the flags say whether polkit may ask, so they are needed first */
    r = sd_varlink_dispatch(link, parameters, dispatch_table, &transaction_flags);
    if (r != 0)
        return r;

    r = mfd_policy_check_authorized(link,
                                    MF_VARLINK_METHOD_CLEANUP,
                                    MFD_POLKIT_ACTION_REMOVE,
                                    transaction_flags,
                                    &peer_uid);
    if (r != 0)
        return r;

    g_info("User %u requested to clean up", (unsigned int) peer_uid);

    return mfd_service_remove(service, link, NULL, NULL, (flags & SD_VARLINK_METHOD_MORE) != 0);
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
    g_auto(MfdErrorDetail) detail = {};
    g_autoptr(GPtrArray) marked = NULL;
    g_autoptr(sd_json_variant) components = NULL;
    g_autoptr(GError) error = NULL;
    MfInstallMode mode;
    uid_t peer_uid;
    int r;

    /* the flags say whether polkit may ask, so they are needed first */
    r = sd_varlink_dispatch(link, parameters, dispatch_table, &p);
    if (r != 0)
        return r;

    r = mfd_policy_check_authorized(link,
                                    MF_VARLINK_METHOD_MARK_COMPONENT,
                                    MFD_POLKIT_ACTION_MARK,
                                    p.flags,
                                    &peer_uid);
    if (r != 0)
        return r;
    if (!mf_component_id_is_valid(p.component_id))
        return sd_varlink_error_invalid_parameter_name(link, "componentId");
    if (p.version != NULL && !mf_version_is_valid(p.version))
        return sd_varlink_error_invalid_parameter_name(link, "version");
    mode = mf_install_mode_from_string(p.mode);
    if (mode == MF_INSTALL_MODE_UNKNOWN)
        return sd_varlink_error_invalid_parameter_name(link, "mode");

    g_info("User %u requested to mark %s %s as %s",
           (unsigned int) peer_uid,
           p.component_id,
           p.version ? p.version : "",
           p.mode);

    marked = mfd_manager_mark(service->manager, p.component_id, p.version, mode, &detail, &error);
    if (marked == NULL)
        return mfd_service_reply_error(link, error, &detail);
    if ((r = mfd_varlink_build_components_array(marked, &components)) < 0)
        return r;

    return sd_varlink_replybo(
        link,
        SD_JSON_BUILD_PAIR("done",
                           SD_JSON_BUILD_OBJECT(SD_JSON_BUILD_PAIR_VARIANT("components", components))));
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
                                            SD_VARLINK_SERVER_ALLOW_FD_PASSING_INPUT,
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
    int r;

    r = sd_event_add_time_relative(sd_varlink_server_get_event(service->server),
                                   &service->idle_timer,
                                   CLOCK_MONOTONIC,
                                   IDLE_TIMEOUT_USEC,
                                   G_USEC_PER_SEC,
                                   mfd_on_idle_timeout,
                                   NULL);
    if (r < 0)
        return r;

    sd_varlink_server_bind_connect(service->server, mfd_on_connect);
    sd_varlink_server_bind_disconnect(service->server, mfd_on_disconnect);
    return 0;
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
