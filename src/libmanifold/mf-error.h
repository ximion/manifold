/*
 * Copyright (C) 2024-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#if !defined(__MANIFOLD_H_INSIDE__) && !defined(MF_COMPILATION)
#error "Only <manifold.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/**
 * MfError:
 * @MF_ERROR_FAILED: Generic failure.
 * @MF_ERROR_NO_SERVICE: The Manifold service could not be reached.
 * @MF_ERROR_NOT_IMPLEMENTED: The requested operation is not implemented.
 * @MF_ERROR_PERMISSION_DENIED: The caller is not allowed to do this.
 * @MF_ERROR_INVALID_BUNDLE: The file is not a Manifold bundle.
 * @MF_ERROR_VERIFICATION_FAILED: The bundle does not match what its index says.
 * @MF_ERROR_MISSING_DEPENDENCY: A core, platform or kit the application needs is not installable.
 * @MF_ERROR_ALREADY_INSTALLED: This version of the component is installed already.
 * @MF_ERROR_NO_SUCH_COMPONENT: The component is not installed.
 * @MF_ERROR_COMPONENT_IN_USE: An installed component builds on this component.
 *
 * Errors that can happen when managing the Manifold installation of a host.
 */
typedef enum {
    MF_ERROR_FAILED,
    MF_ERROR_NO_SERVICE,
    MF_ERROR_NOT_IMPLEMENTED,
    MF_ERROR_PERMISSION_DENIED,
    MF_ERROR_INVALID_BUNDLE,
    MF_ERROR_VERIFICATION_FAILED,
    MF_ERROR_MISSING_DEPENDENCY,
    MF_ERROR_ALREADY_INSTALLED,
    MF_ERROR_NO_SUCH_COMPONENT,
    MF_ERROR_COMPONENT_IN_USE,
    /*< private >*/
    MF_ERROR_LAST
} MfError;

#define MF_ERROR mf_error_quark()
GQuark mf_error_quark(void);

G_END_DECLS
