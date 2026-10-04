/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib-object.h>

#include "mfr-tree.h"

G_BEGIN_DECLS

#define MFR_TYPE_COMPOSER (mfr_composer_get_type())
G_DECLARE_FINAL_TYPE(MfrComposer, mfr_composer, MFR, COMPOSER, GObject)

MfrComposer *mfr_composer_new(GError **error);

MfrTree     *mfr_composer_prepare(MfrComposer *composer,
                                  const char  *component_id,
                                  GError     **error);
GList       *mfr_composer_list_trees(MfrComposer *composer);
gboolean     mfr_composer_refresh_capture(MfrComposer *composer,
                                          GError     **error);

G_END_DECLS
