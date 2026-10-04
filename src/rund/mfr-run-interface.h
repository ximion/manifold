/*
 * Copyright (C) 2024-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "config.h"

#include <systemd/sd-varlink-idl.h>

/* methods */
#define MF_RUN_METHOD_PREPARE_LAUNCH  MF_RUN_INTERFACE ".PrepareLaunch"
#define MF_RUN_METHOD_REFRESH_CAPTURE MF_RUN_INTERFACE ".RefreshCapture"
#define MF_RUN_METHOD_LIST_TREES      MF_RUN_INTERFACE ".ListTrees"

/* errors */
#define MF_RUN_ERROR_NO_SUCH_COMPONENT_ID  MF_RUN_INTERFACE ".NoSuchComponent"
#define MF_RUN_ERROR_COMPONENT_REVOKED_ID  MF_RUN_INTERFACE ".ComponentRevoked"
#define MF_RUN_ERROR_COMPOSITION_FAILED_ID MF_RUN_INTERFACE ".CompositionFailed"
#define MF_RUN_ERROR_NO_TREES_ID           MF_RUN_INTERFACE ".NoTrees"

extern const sd_varlink_interface vl_interface_org_freedesktop_manifold_run;
