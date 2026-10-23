/*
 * Copyright (C) 2024-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib-object.h>
#include <systemd/sd-json.h>

G_BEGIN_DECLS

/**
 * MfrLayerKind:
 *
 * The layers of a mount tree, lowest first. Core, platform and kit are the
 * runtime of an application. Platform and kit are optional.
 */
typedef enum {
    MFR_LAYER_CORE,
    MFR_LAYER_PLATFORM,
    MFR_LAYER_KIT,
    MFR_LAYER_APP,
    /*< private >*/
    MFR_LAYER_LAST
} MfrLayerKind;

#define MFR_LAYER_COUNT (MFR_LAYER_LAST)

/**
 * MfrImage:
 *
 * What is needed to mount the verified filesystem of an image.
 */
typedef struct {
    char   *image_path;
    char   *verity_hash;
    guint64 data_offset; /* where the filesystem partition starts in the image */
    guint64 hash_offset; /* where the verity partition starts in the image */
} MfrImage;

/**
 * MfrLayer:
 *
 * One image of a mount tree, and what is needed to mount it. A layer the
 * application does not have has no image: its verity hash is %NULL.
 */
typedef struct {
    char    *component_id;
    char    *version;
    MfrImage image;
} MfrLayer;

#define MFR_TYPE_APP (mfr_app_get_type())
G_DECLARE_FINAL_TYPE(MfrApp, mfr_app, MFR, APP, GObject)

/**
 * MfrApp:
 *
 * An installed application, as its directory in the store describes it:
 * which images it consists of, and the order they are layered in.
 * All of the fields here are read-only.
 */
struct _MfrApp {
    GObject  parent_instance;

    char    *component_id;
    char    *version;
    char    *command;    /* the program to run, below /app/bin */
    char    *arch_tuple; /* multiarch tuple of the application */
    MfrLayer layers[MFR_LAYER_COUNT];
};

MfrApp *mfr_app_load(const char *component_id,
                     GError    **error);

int     mfr_app_append_layers_json(const MfrApp     *app,
                                   sd_json_variant **array);

G_END_DECLS
