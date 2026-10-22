/*
 * Copyright (C) 2024-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define MFR_CAPTURE_DIR MF_RUN_CACHE_DIR "/capture"

#define MFR_TYPE_CAPTURE (mfr_capture_get_type())
G_DECLARE_FINAL_TYPE(MfrCapture, mfr_capture, MFR, CAPTURE, GObject)

MfrCapture        *mfr_capture_new(const char *core_hash,
                                   const char *arch_tuple);
gboolean           mfr_capture_ensure(MfrCapture *capture,
                                      const char *core_mount,
                                      GError    **error);

const char        *mfr_capture_get_id(const MfrCapture *capture);
const char        *mfr_capture_get_overrides_dir(const MfrCapture *capture);
const char *const *mfr_capture_get_environment(const MfrCapture *capture);
gboolean           mfr_capture_is_fresh(const MfrCapture *capture);

void               mfr_capture_check_all(GHashTable *used_ids);
gboolean           mfr_capture_discard_all(GError **error);

G_END_DECLS
