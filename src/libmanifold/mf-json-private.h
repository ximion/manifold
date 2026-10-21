/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib.h>
#include <systemd/sd-json.h>

#include "mf-error.h"
#include "mf-macros-private.h"

MF_BEGIN_PRIVATE_DECLS

sd_json_variant *mf_json_parse_object(const char *data,
                                      const char *context,
                                      MfError     code,
                                      GError    **error);

const char      *mf_json_get_string_field(sd_json_variant *object,
                                          const char      *context,
                                          const char      *key,
                                          MfError          code,
                                          GError         **error);
gboolean         mf_json_get_unsigned_field(sd_json_variant *object,
                                            const char      *context,
                                            const char      *key,
                                            MfError          code,
                                            guint64         *ret,
                                            GError         **error);
gboolean         mf_json_get_boolean_field(sd_json_variant *object,
                                           const char      *context,
                                           const char      *key,
                                           MfError          code,
                                           gboolean        *ret,
                                           GError         **error);
const char      *mf_json_get_hash_field(sd_json_variant *object,
                                        const char      *context,
                                        const char      *key,
                                        MfError          code,
                                        GError         **error);

/* more lenient readers */
MF_INTERNAL_VISIBLE
sd_json_variant *mf_json_parse_file(const char *path,
                                    GError    **error);
MF_INTERNAL_VISIBLE
const char *mf_json_get_string(sd_json_variant *object,
                               const char      *key);

MF_INTERNAL_VISIBLE
gboolean mf_json_write_file(const char      *path,
                            sd_json_variant *json,
                            GError         **error);

MF_END_PRIVATE_DECLS
