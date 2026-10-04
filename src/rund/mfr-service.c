/*
 * Copyright (C) 2024-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"
#include "mfr-service.h"

#include <errno.h>
#include <systemd/sd-json.h>
#include <systemd/sd-varlink.h>
#include <libglnx.h>
#include <manifold.h>

#include "mf-sdautoptr-private.h"
#include "mf-varlink-util.h"
#include "mfr-composer.h"
#include "mfr-run-interface.h"

/**
 * MfrService:
 *
 * The Varlink service of manifold-rund.
 */
struct _MfrService {
    GObject parent_instance;

    sd_varlink_server *server;
    MfrComposer *composer;
};

G_DEFINE_FINAL_TYPE(MfrService, mfr_service, G_TYPE_OBJECT)

static void
mfr_service_init(MfrService *service)
{
}

static void
mfr_service_finalize(GObject *object)
{
    MfrService *service = MFR_SERVICE(object);

    sd_varlink_server_unref(service->server);
    g_clear_object(&service->composer);

    G_OBJECT_CLASS(mfr_service_parent_class)->finalize(object);
}

static void
mfr_service_class_init(MfrServiceClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = mfr_service_finalize;
}

/**
 * mfr_varlink_dispatch_no_parameters:
 *
 * Reject any parameters for a method that does not take any.
 *
 * Returns: 0 if the call has no parameters, or a negative errno once we replied with an error
 */
static int
mfr_varlink_dispatch_no_parameters(sd_varlink *link, sd_json_variant *parameters)
{
    static const sd_json_dispatch_field dispatch_table[] = {
        {},
    };

    if (parameters == NULL)
        return 0;

    return sd_varlink_dispatch(link, parameters, dispatch_table, NULL);
}

/**
 * mfr_varlink_reply_permission_denied:
 */
static int
mfr_varlink_reply_permission_denied(sd_varlink *link, const char *method, uid_t peer_uid)
{
    g_info("Denied call of %s by user %u", method, (unsigned int) peer_uid);
    return sd_varlink_error(link, SD_VARLINK_ERROR_PERMISSION_DENIED, NULL);
}

/**
 * mfr_varlink_reply_error:
 *
 * Tell the caller why a component can not be launched.
 */
static int
mfr_varlink_reply_error(sd_varlink *link, const char *component_id, const GError *error)
{
    if (g_error_matches(error, MF_ERROR, MF_ERROR_NOT_FOUND))
        return sd_varlink_errorbo(link,
                                  MF_RUN_ERROR_NO_SUCH_COMPONENT_ID,
                                  SD_JSON_BUILD_PAIR_STRING("componentId", component_id));
    if (g_error_matches(error, MF_ERROR, MF_ERROR_REVOKED))
        return sd_varlink_errorbo(link,
                                  MF_RUN_ERROR_COMPONENT_REVOKED_ID,
                                  SD_JSON_BUILD_PAIR_STRING("componentId", component_id));

    g_warning("Unable to prepare %s: %s", component_id, error->message);
    return sd_varlink_errorbo(link,
                              MF_RUN_ERROR_COMPOSITION_FAILED_ID,
                              SD_JSON_BUILD_PAIR_STRING("componentId", component_id),
                              SD_JSON_BUILD_PAIR_STRING("message", error->message));
}

/**
 * mfr_varlink_method_prepare_launch:
 */
static int
mfr_varlink_method_prepare_launch(sd_varlink *link,
                                  sd_json_variant *parameters,
                                  sd_varlink_method_flags_t flags,
                                  void *userdata)
{
    static const sd_json_dispatch_field dispatch_table[] = {
        {"componentId", SD_JSON_VARIANT_STRING, sd_json_dispatch_const_string, 0, SD_JSON_MANDATORY},
        {},
    };
    MfrService *service = userdata;
    g_autoptr(GError) error = NULL;
    const char *component_id = NULL;
    g_autoptr(MfrTree) tree = NULL;
    glnx_autofd int mount_fd = -1;
    uid_t peer_uid;

    int r = mf_varlink_get_peer_uid(link, &peer_uid);
    if (r < 0)
        return r;

    r = sd_varlink_dispatch(link, parameters, dispatch_table, &component_id);
    if (r != 0)
        return r;
    if (!mf_component_id_is_valid(component_id))
        return sd_varlink_error_invalid_parameter_name(link, "componentId");

    g_info("User %u requested to launch %s", (unsigned int) peer_uid, component_id);

    tree = mfr_composer_prepare(service->composer, component_id, &error);
    if (tree == NULL)
        return mfr_varlink_reply_error(link, component_id, error);
    if (!mfr_tree_clone(tree, &mount_fd, &error))
        return mfr_varlink_reply_error(link, component_id, error);

    int mount_index = sd_varlink_push_fd(link, mount_fd);
    if (mount_index < 0) {
        g_warning("Unable to pass the tree to user %u: %s",
                  (unsigned int) peer_uid,
                  g_strerror(-mount_index));
        return sd_varlink_error_errno(link, mount_index);
    }
    mount_fd = -1;

    return sd_varlink_replybo(
        link,
        SD_JSON_BUILD_PAIR_INTEGER("mountFileDescriptor", mount_index),
        SD_JSON_BUILD_PAIR_STRING("executable", mfr_tree_get_executable(tree)),
        SD_JSON_BUILD_PAIR_STRV("environment", (char **) mfr_tree_get_environment(tree)));
}

/**
 * mfr_varlink_method_refresh_capture:
 */
static int
mfr_varlink_method_refresh_capture(sd_varlink *link,
                                   sd_json_variant *parameters,
                                   sd_varlink_method_flags_t flags,
                                   void *userdata)
{
    MfrService *service = userdata;
    g_autoptr(GError) error = NULL;
    uid_t peer_uid;

    int r = mf_varlink_get_peer_uid(link, &peer_uid);
    if (r < 0)
        return r;
    if (!mf_peer_is_privileged(peer_uid))
        return mfr_varlink_reply_permission_denied(link, MF_RUN_METHOD_REFRESH_CAPTURE, peer_uid);

    r = mfr_varlink_dispatch_no_parameters(link, parameters);
    if (r != 0)
        return r;

    g_info("User %u requested to refresh the host capture", (unsigned int) peer_uid);

    if (!mfr_composer_refresh_capture(service->composer, &error)) {
        g_warning("%s", error->message);
        return sd_varlink_error_errno(link, -EIO);
    }

    return sd_varlink_reply(link, NULL);
}

/**
 * mfr_varlink_method_list_trees:
 */
static int
mfr_varlink_method_list_trees(sd_varlink *link,
                              sd_json_variant *parameters,
                              sd_varlink_method_flags_t flags,
                              void *userdata)
{
    MfrService *service = userdata;
    g_autoptr(GList) trees = NULL;
    uid_t peer_uid;

    int r = mf_varlink_get_peer_uid(link, &peer_uid);
    if (r < 0)
        return r;
    if (!mf_peer_is_privileged(peer_uid))
        return mfr_varlink_reply_permission_denied(link, MF_RUN_METHOD_LIST_TREES, peer_uid);

    r = mfr_varlink_dispatch_no_parameters(link, parameters);
    if (r != 0)
        return r;

    g_info("User %u requested the list of mount trees", (unsigned int) peer_uid);

    trees = mfr_composer_list_trees(service->composer);
    if (trees == NULL)
        return sd_varlink_error(link, MF_RUN_ERROR_NO_TREES_ID, NULL);
    if (!(flags & SD_VARLINK_METHOD_MORE))
        return sd_varlink_error(link, SD_VARLINK_ERROR_EXPECTED_MORE, NULL);

    for (GList *l = trees; l != NULL; l = l->next) {
        const MfrApp *app = mfr_tree_get_app(l->data);
        g_autoptr(sd_json_variant) layers = NULL;

        r = mfr_app_append_layers_json(app, &layers);
        if (r < 0)
            return r;
        if (l->next != NULL)
            r = sd_varlink_notifybo(link,
                                    SD_JSON_BUILD_PAIR_STRING("componentId", app->component_id),
                                    SD_JSON_BUILD_PAIR_VARIANT("layers", layers));
        else
            r = sd_varlink_replybo(link,
                                   SD_JSON_BUILD_PAIR_STRING("componentId", app->component_id),
                                   SD_JSON_BUILD_PAIR_VARIANT("layers", layers));
        if (r < 0)
            return r;
    }

    return 0;
}

/**
 * mfr_service_new:
 * @event: The event loop to run the service in.
 * @error: A #GError
 *
 * Create the service, and the composer it serves from. It does not listen
 * for connections yet.
 */
MfrService *
mfr_service_new(sd_event *event, GError **error)
{
    g_autoptr(MfrService) service = g_object_new(MFR_TYPE_SERVICE, NULL);
    int r;

    service->composer = mfr_composer_new(error);
    if (service->composer == NULL)
        return NULL;

    service->server = mf_varlink_server_new(event,
                                            "manifold-rund",
                                            SD_VARLINK_SERVER_ALLOW_FD_PASSING_OUTPUT,
                                            &vl_interface_org_freedesktop_manifold_run,
                                            service,
                                            error);
    if (service->server == NULL)
        return NULL;

    r = sd_varlink_server_bind_method_many(service->server,
                                           MF_RUN_METHOD_PREPARE_LAUNCH,
                                           mfr_varlink_method_prepare_launch,
                                           MF_RUN_METHOD_REFRESH_CAPTURE,
                                           mfr_varlink_method_refresh_capture,
                                           MF_RUN_METHOD_LIST_TREES,
                                           mfr_varlink_method_list_trees);
    if (r < 0) {
        mf_set_error_from_errno(error, r, "Unable to bind Varlink methods");
        return NULL;
    }

    return g_steal_pointer(&service);
}

/**
 * mfr_service_listen:
 * @service: The service.
 * @socket_path: (nullable): Path of the socket to listen on, or %NULL
 * @error: A #GError
 *
 * Listen for connections. If no socket path is given, we listen on the socket
 * we were activated with, or on our default socket if we were not started
 * via socket activation.
 */
gboolean
mfr_service_listen(MfrService *service, const char *socket_path, GError **error)
{
    gboolean activated;

    return mf_varlink_server_listen(service->server, socket_path, MF_RUN_SOCKET_PATH, &activated, error);
}
