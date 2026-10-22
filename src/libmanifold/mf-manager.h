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

/**
 * MfTransactionFlags:
 * @MF_TRANSACTION_FLAG_NONE: No flags.
 * @MF_TRANSACTION_FLAG_INTERACTIVE: The service may ask interactive questions, e.g. for PolicyKit authorization
 *
 * General transaction flags.
 */
typedef enum {
    MF_TRANSACTION_FLAG_NONE        = 0,
    MF_TRANSACTION_FLAG_INTERACTIVE = 1 << 0,
    /*< private >*/
    MF_TRANSACTION_FLAG_LAST
} MfTransactionFlags;

#define MF_TYPE_TRANSACTION_FLAGS (mf_transaction_flags_get_type())
GType mf_transaction_flags_get_type(void);

#define MF_TYPE_MANAGER (mf_manager_get_type())
G_DECLARE_DERIVABLE_TYPE(MfManager, mf_manager, MF, MANAGER, GObject)

struct _MfManagerClass {
    GObjectClass parent_class;
    /*< private >*/
    gpointer     padding[8];
};

MfManager         *mf_manager_new(void);

const char        *mf_manager_get_socket_path(MfManager *manager);
void               mf_manager_set_socket_path(MfManager  *manager,
                                              const char *socket_path);

MfTransactionFlags mf_manager_get_transaction_flags(MfManager *manager);
void               mf_manager_set_transaction_flags(MfManager         *manager,
                                                    MfTransactionFlags flags);

void               mf_manager_install_file_async(MfManager          *manager,
                                                 int                 fd,
                                                 MfProgressFunc      progress_func,
                                                 gpointer            progress_data,
                                                 GDestroyNotify      progress_data_destroy,
                                                 GCancellable       *cancellable,
                                                 GAsyncReadyCallback callback,
                                                 gpointer            user_data);
MfResult          *mf_manager_install_file_finish(MfManager    *manager,
                                                  GAsyncResult *result,
                                                  GError      **error);
MfResult          *mf_manager_install_file(MfManager     *manager,
                                           int            fd,
                                           MfProgressFunc progress_func,
                                           gpointer       progress_data,
                                           GCancellable  *cancellable,
                                           GError       **error);

void               mf_manager_remove_component_async(MfManager          *manager,
                                                     const char         *component_id,
                                                     const char         *version,
                                                     MfProgressFunc      progress_func,
                                                     gpointer            progress_data,
                                                     GDestroyNotify      progress_data_destroy,
                                                     GCancellable       *cancellable,
                                                     GAsyncReadyCallback callback,
                                                     gpointer            user_data);
MfResult          *mf_manager_remove_component_finish(MfManager    *manager,
                                                      GAsyncResult *result,
                                                      GError      **error);
MfResult          *mf_manager_remove_component(MfManager     *manager,
                                               const char    *component_id,
                                               const char    *version,
                                               MfProgressFunc progress_func,
                                               gpointer       progress_data,
                                               GCancellable  *cancellable,
                                               GError       **error);

void               mf_manager_cleanup_async(MfManager          *manager,
                                            MfProgressFunc      progress_func,
                                            gpointer            progress_data,
                                            GDestroyNotify      progress_data_destroy,
                                            GCancellable       *cancellable,
                                            GAsyncReadyCallback callback,
                                            gpointer            user_data);
MfResult          *mf_manager_cleanup_finish(MfManager    *manager,
                                             GAsyncResult *result,
                                             GError      **error);
MfResult          *mf_manager_cleanup(MfManager     *manager,
                                      MfProgressFunc progress_func,
                                      gpointer       progress_data,
                                      GCancellable  *cancellable,
                                      GError       **error);

void               mf_manager_mark_component_async(MfManager          *manager,
                                                   const char         *component_id,
                                                   const char         *version,
                                                   MfInstallMode       mode,
                                                   GCancellable       *cancellable,
                                                   GAsyncReadyCallback callback,
                                                   gpointer            user_data);
GPtrArray         *mf_manager_mark_component_finish(MfManager    *manager,
                                                    GAsyncResult *result,
                                                    GError      **error);
GPtrArray         *mf_manager_mark_component(MfManager    *manager,
                                             const char   *component_id,
                                             const char   *version,
                                             MfInstallMode mode,
                                             GCancellable *cancellable,
                                             GError      **error);

G_END_DECLS
