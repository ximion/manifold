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

G_BEGIN_DECLS

/*
 * This is internal API of libmanifold that we publish for convenience.
 * It is unstable though and might change at any time.
 */

/**
 * MF_BUNDLE_CATALOG_DIR:
 *
 * The directory of a bundle that holds its AppStream catalog (with the
 * catalog XML, the icons and the screenshot media)
 */
#define MF_BUNDLE_CATALOG_DIR "catalog"

/**
 * MF_BUNDLE_CATALOG_ORIGIN:
 *
 * The origin of the AppStream catalog of a bundle, and of the components
 * installed from local bundles.
 */
#define MF_BUNDLE_CATALOG_ORIGIN "manifold-local"

/**
 * MF_SIGNATURE_NAMESPACE_BUNDLE:
 *
 * The SSHSig namespace the index of a bundle is signed in.
 */
#define MF_SIGNATURE_NAMESPACE_BUNDLE "manifold.bundle"

/**
 * MF_SIGNATURE_NAMESPACE_REPO:
 *
 * The SSHSig namespace the metadata of a repository is signed in.
 */
#define MF_SIGNATURE_NAMESPACE_REPO "manifold.repo"

/**
 * MF_SIGNING_KEY_ENV:
 *
 * The environment variable that can hold a private key to sign with.
 */
#define MF_SIGNING_KEY_ENV "MANIFOLD_SIGNING_KEY"

G_END_DECLS
