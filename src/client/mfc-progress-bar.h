/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct MfcProgressBar MfcProgressBar;

MfcProgressBar               *mfc_progress_bar_new(gboolean is_tty);
void                          mfc_progress_bar_free(MfcProgressBar *bar);

const char                   *mfc_progress_bar_get_label(MfcProgressBar *bar);
void                          mfc_progress_bar_start(MfcProgressBar *bar,
                                                     const char     *label);
void                          mfc_progress_bar_update(MfcProgressBar *bar,
                                                      double          percent,
                                                      guint64         bytes_done);
void                          mfc_progress_bar_finish(MfcProgressBar *bar);

G_END_DECLS
