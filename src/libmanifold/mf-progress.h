/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#if !defined(__MANIFOLD_H_INSIDE__) && !defined(MF_COMPILATION)
#error "Only <manifold.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * MfAction:
 * @MF_ACTION_UNKNOWN: Unknown action.
 * @MF_ACTION_INSTALL: A component is installed.
 * @MF_ACTION_REMOVE: A component is removed.
 * @MF_ACTION_CLEANUP: A component nothing uses is removed.
 *
 * What action Manifold performs.
 */
typedef enum {
    MF_ACTION_UNKNOWN,
    MF_ACTION_INSTALL,
    MF_ACTION_REMOVE,
    MF_ACTION_CLEANUP,
    /*< private >*/
    MF_ACTION_LAST
} MfAction;

const char                    *mf_action_to_string(MfAction action);
MfAction                       mf_action_from_string(const char *action_str);

typedef struct _MfItemProgress MfItemProgress;

#define MF_TYPE_ITEM_PROGRESS (mf_item_progress_get_type())
GType           mf_item_progress_get_type(void);

MfItemProgress *mf_item_progress_copy(const MfItemProgress *item);
void            mf_item_progress_free(MfItemProgress *item);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(MfItemProgress,
                              mf_item_progress_free)

MfAction                   mf_item_progress_get_action(const MfItemProgress *item);
const char                *mf_item_progress_get_subject(const MfItemProgress *item);
const char                *mf_item_progress_get_version(const MfItemProgress *item);
double                     mf_item_progress_get_percent(const MfItemProgress *item);
guint64                    mf_item_progress_get_bytes_done(const MfItemProgress *item);
guint64                    mf_item_progress_get_bytes_total(const MfItemProgress *item);

void                       mf_item_progress_set_percent(MfItemProgress *item,
                                                        double          percent);
void                       mf_item_progress_set_bytes_done(MfItemProgress *item,
                                                           guint64         bytes_done);
void                       mf_item_progress_set_bytes_total(MfItemProgress *item,
                                                            guint64         bytes_total);

typedef struct _MfProgress MfProgress;

#define MF_TYPE_PROGRESS (mf_progress_get_type())
GType       mf_progress_get_type(void);

MfProgress *mf_progress_copy(const MfProgress *progress);
void        mf_progress_free(MfProgress *progress);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(MfProgress,
                              mf_progress_free)

MfProgress           *mf_progress_new(void);
double                mf_progress_get_percent(const MfProgress *progress);
const MfItemProgress *mf_progress_get_item(const MfProgress *progress);

void                  mf_progress_set_percent(MfProgress *progress,
                                              double      percent);
void                  mf_progress_take_item(MfProgress     *progress,
                                            MfItemProgress *item);

/**
 * MfProgressFunc:
 * @progress: how far the operation is, valid while the function runs
 * @user_data: what was passed along with the function
 *
 * Called while an operation runs, whenever the service reports progress.
 */
typedef void (*MfProgressFunc)(const MfProgress *progress,
                               gpointer          user_data);

G_END_DECLS
