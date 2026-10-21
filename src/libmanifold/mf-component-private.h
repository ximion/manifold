/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <systemd/sd-json.h>

#include "mf-component.h"
#include "mf-macros-private.h"

MF_BEGIN_PRIVATE_DECLS

MF_INTERNAL_VISIBLE
gboolean mf_component_kind_is_always_auto(MfComponentKind kind,
                                          gboolean        sdk);

MF_INTERNAL_VISIBLE
MfComponent *mf_component_new(const char     *component_id,
                              MfComponentKind kind,
                              const char     *version,
                              guint64         revision,
                              gboolean        sdk,
                              const char     *identity,
                              const char     *verity_hash,
                              gboolean        current,
                              MfInstallMode   install_mode,
                              const char     *origin,
                              const char     *dir);
MfComponent *mf_component_new_from_reply(sd_json_variant *variant,
                                         GError         **error);
GPtrArray   *mf_component_array_from_reply(sd_json_variant *list,
                                           GError         **error);

void         mf_component_set_manifest(MfComponent *component,
                                       MfManifest  *manifest);

MF_END_PRIVATE_DECLS
