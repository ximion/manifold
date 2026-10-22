/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "config.h"

#include <systemd/sd-varlink-idl.h>

#include "mf-macros-private.h"

MF_BEGIN_PRIVATE_DECLS

/*
 * The org.freedesktop.manifold Varlink interface, shared by the library that
 * calls it and the daemon that provides it.
 */

/* methods */
#define MF_VARLINK_METHOD_INSTALL_FILE     MF_INTERFACE ".InstallFile"
#define MF_VARLINK_METHOD_REMOVE_COMPONENT MF_INTERFACE ".RemoveComponent"
#define MF_VARLINK_METHOD_CLEANUP          MF_INTERFACE ".Cleanup"
#define MF_VARLINK_METHOD_MARK_COMPONENT   MF_INTERFACE ".MarkComponent"

/* transaction flags */
#define MF_VARLINK_TRANSACTION_FLAG_ALLOW_INTERACTIVE "allowInteractive"

/* errors */
#define MF_VARLINK_ERROR_FAILED              MF_INTERFACE ".Failed"
#define MF_VARLINK_ERROR_INVALID_BUNDLE      MF_INTERFACE ".InvalidBundle"
#define MF_VARLINK_ERROR_VERIFICATION_FAILED MF_INTERFACE ".VerificationFailed"
#define MF_VARLINK_ERROR_MISSING_BASE        MF_INTERFACE ".MissingBase"
#define MF_VARLINK_ERROR_ALREADY_INSTALLED   MF_INTERFACE ".AlreadyInstalled"
#define MF_VARLINK_ERROR_NO_SUCH_COMPONENT   MF_INTERFACE ".NoSuchComponent"
#define MF_VARLINK_ERROR_COMPONENT_IN_USE    MF_INTERFACE ".ComponentInUse"
#define MF_VARLINK_ERROR_WRONG_ARCHITECTURE  MF_INTERFACE ".WrongArchitecture"
#define MF_VARLINK_ERROR_ID_CONFLICT         MF_INTERFACE ".IdConflict"

/* exported so the daemon can serve it */
MF_INTERNAL_VISIBLE
extern const sd_varlink_interface vl_interface_org_freedesktop_manifold;

MF_END_PRIVATE_DECLS
