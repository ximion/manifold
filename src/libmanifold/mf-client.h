/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#if !defined(__MANIFOLD_H_INSIDE__) && !defined(MF_COMPILATION)
#error "Only <manifold.h> can be included directly."
#endif

#include <gio/gio.h>
#include <glib-object.h>

#include "mf-component.h"
#include "mf-error.h"
#include "mf-progress.h"
#include "mf-result.h"

G_BEGIN_DECLS

#define MF_TYPE_CLIENT (mf_client_get_type())
G_DECLARE_DERIVABLE_TYPE(MfClient, mf_client, MF, CLIENT, GObject)

struct _MfClientClass {
    GObjectClass parent_class;
    /*< private >*/
    gpointer     padding[8];
};

MfClient   *mf_client_new(void);

const char *mf_client_get_socket_path(MfClient *client);
void        mf_client_set_socket_path(MfClient   *client,
                                      const char *socket_path);

void        mf_client_install_file_async(MfClient           *client,
                                         int                 fd,
                                         MfProgressFunc      progress_func,
                                         gpointer            progress_data,
                                         GCancellable       *cancellable,
                                         GAsyncReadyCallback callback,
                                         gpointer            user_data);
MfResult   *mf_client_install_file_finish(MfClient     *client,
                                          GAsyncResult *result,
                                          GError      **error);
MfResult   *mf_client_install_file(MfClient      *client,
                                   int            fd,
                                   MfProgressFunc progress_func,
                                   gpointer       progress_data,
                                   GCancellable  *cancellable,
                                   GError       **error);

void        mf_client_remove_component_async(MfClient           *client,
                                             const char         *component_id,
                                             const char         *version,
                                             MfProgressFunc      progress_func,
                                             gpointer            progress_data,
                                             GCancellable       *cancellable,
                                             GAsyncReadyCallback callback,
                                             gpointer            user_data);
MfResult   *mf_client_remove_component_finish(MfClient     *client,
                                              GAsyncResult *result,
                                              GError      **error);
MfResult   *mf_client_remove_component(MfClient      *client,
                                       const char    *component_id,
                                       const char    *version,
                                       MfProgressFunc progress_func,
                                       gpointer       progress_data,
                                       GCancellable  *cancellable,
                                       GError       **error);

void        mf_client_cleanup_async(MfClient           *client,
                                    MfProgressFunc      progress_func,
                                    gpointer            progress_data,
                                    GCancellable       *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer            user_data);
MfResult   *mf_client_cleanup_finish(MfClient     *client,
                                     GAsyncResult *result,
                                     GError      **error);
MfResult   *mf_client_cleanup(MfClient      *client,
                              MfProgressFunc progress_func,
                              gpointer       progress_data,
                              GCancellable  *cancellable,
                              GError       **error);

void        mf_client_list_components_async(MfClient           *client,
                                            const char         *component_id,
                                            MfComponentKind     kind,
                                            GCancellable       *cancellable,
                                            GAsyncReadyCallback callback,
                                            gpointer            user_data);
GPtrArray  *mf_client_list_components_finish(MfClient     *client,
                                             GAsyncResult *result,
                                             GError      **error);
GPtrArray  *mf_client_list_components(MfClient       *client,
                                      const char     *component_id,
                                      MfComponentKind kind,
                                      GCancellable   *cancellable,
                                      GError        **error);

G_END_DECLS
