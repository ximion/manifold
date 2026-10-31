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
 * MfSignatureScope:
 * @MF_SIGNATURE_SCOPE_UNKNOWN: Not a known scope.
 * @MF_SIGNATURE_SCOPE_BUNDLE: The index of a bundle.
 * @MF_SIGNATURE_SCOPE_REPO: The metadata of a repository.
 *
 * What a signature is for. It is only valid in the scope it was made in.
 */
typedef enum {
    MF_SIGNATURE_SCOPE_UNKNOWN,
    MF_SIGNATURE_SCOPE_BUNDLE,
    MF_SIGNATURE_SCOPE_REPO,
    /*< private >*/
    MF_SIGNATURE_SCOPE_LAST
} MfSignatureScope;

/**
 * MF_SIGNING_KEY_DEFAULT_NAME:
 *
 * The name of the key in the key directory of the user that we sign with
 * if no other key was selected.
 */
#define MF_SIGNING_KEY_DEFAULT_NAME "default"

#define MF_TYPE_SIGNER (mf_signer_get_type())
G_DECLARE_DERIVABLE_TYPE(MfSigner, mf_signer, MF, SIGNER, GObject)

struct _MfSignerClass {
    GObjectClass parent_class;
    /*< private >*/
    gpointer     padding[8];
};

MfSigner   *mf_signer_new_for_key_file(const char *path);
MfSigner   *mf_signer_new_for_key_data(const char *private_key);
MfSigner   *mf_signer_new_for_agent(const char *public_key);
MfSigner   *mf_signer_new_for_key(const char *key,
                                  GError    **error);
MfSigner   *mf_signer_new_default(GError **error);

char       *mf_signer_build_key_dir(void);
GPtrArray  *mf_signer_list_keys(GError **error);

const char *mf_signer_get_name(MfSigner *signer);
const char *mf_signer_get_public_key(MfSigner *signer);
const char *mf_signer_get_fingerprint(MfSigner *signer);

gboolean    mf_signer_check_key(MfSigner *signer,
                                GError  **error);
GBytes     *mf_signer_sign(MfSigner        *signer,
                           MfSignatureScope scope,
                           GBytes          *data,
                           GError         **error);

G_END_DECLS
