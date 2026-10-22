/*
 * Copyright (C) 2024-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

/*
 * Definition of the org.freedesktop.manifold.run Varlink interface.
 */

#define _GNU_SOURCE
#include "mfr-run-interface.h"

/* clang-format off */

static SD_VARLINK_DEFINE_ENUM_TYPE(
    LayerKind,
    SD_VARLINK_FIELD_COMMENT("The root filesystem everything else builds upon."),
    SD_VARLINK_DEFINE_ENUM_VALUE(core),
    SD_VARLINK_FIELD_COMMENT("A platform many applications share, on the core."),
    SD_VARLINK_DEFINE_ENUM_VALUE(platform),
    SD_VARLINK_FIELD_COMMENT("The shared components an application needs beyond the layer below."),
    SD_VARLINK_DEFINE_ENUM_VALUE(kit),
    SD_VARLINK_FIELD_COMMENT("The application itself."),
    SD_VARLINK_DEFINE_ENUM_VALUE(app));

static SD_VARLINK_DEFINE_STRUCT_TYPE(
    Layer,
    SD_VARLINK_FIELD_COMMENT("The kind of this layer."),
    SD_VARLINK_DEFINE_FIELD_BY_TYPE(kind, LayerKind, 0),
    SD_VARLINK_FIELD_COMMENT("Identifier of the component this layer was created from."),
    SD_VARLINK_DEFINE_FIELD(componentId, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("Version of the component."),
    SD_VARLINK_DEFINE_FIELD(version, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("The dm-verity root hash of the image of the component, as a hex encoded string."),
    SD_VARLINK_DEFINE_FIELD(rootHash, SD_VARLINK_STRING, 0));

static SD_VARLINK_DEFINE_METHOD(
    PrepareLaunch,
    SD_VARLINK_FIELD_COMMENT("Identifier of the application to launch."),
    SD_VARLINK_DEFINE_INPUT(componentId, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("File descriptor of the detached mount tree to launch the application in."),
    SD_VARLINK_DEFINE_OUTPUT(mountFileDescriptor, SD_VARLINK_INT, 0),
    SD_VARLINK_FIELD_COMMENT("Absolute path of the program to execute, within the mount tree."),
    SD_VARLINK_DEFINE_OUTPUT(executable, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("Environment variables to set for the application, as NAME=VALUE. "
                             "A variable with an empty value must be unset."),
    SD_VARLINK_DEFINE_OUTPUT(environment, SD_VARLINK_STRING, SD_VARLINK_ARRAY));

static SD_VARLINK_DEFINE_METHOD(
    RefreshCapture);

static SD_VARLINK_DEFINE_METHOD_FULL(
    ListTrees,
    SD_VARLINK_SUPPORTS_MORE,
    SD_VARLINK_FIELD_COMMENT("Identifier of the application the mount tree was composed for."),
    SD_VARLINK_DEFINE_OUTPUT(componentId, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("The layers the mount tree consists of, lowest layer first."),
    SD_VARLINK_DEFINE_OUTPUT_BY_TYPE(layers, Layer, SD_VARLINK_ARRAY));

static SD_VARLINK_DEFINE_ERROR(
    NoSuchComponent,
    SD_VARLINK_FIELD_COMMENT("Identifier of the component that was not found."),
    SD_VARLINK_DEFINE_FIELD(componentId, SD_VARLINK_STRING, 0));

static SD_VARLINK_DEFINE_ERROR(
    ComponentRevoked,
    SD_VARLINK_FIELD_COMMENT("Identifier of the component that was revoked."),
    SD_VARLINK_DEFINE_FIELD(componentId, SD_VARLINK_STRING, 0));

static SD_VARLINK_DEFINE_ERROR(
    CompositionFailed,
    SD_VARLINK_FIELD_COMMENT("Identifier of the component no mount tree could be composed for."),
    SD_VARLINK_DEFINE_FIELD(componentId, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("Description of what went wrong."),
    SD_VARLINK_DEFINE_FIELD(message, SD_VARLINK_STRING, 0));

static SD_VARLINK_DEFINE_ERROR(
    NoTrees);

SD_VARLINK_DEFINE_INTERFACE(
    org_freedesktop_manifold_run,
    MF_RUN_INTERFACE,
    SD_VARLINK_INTERFACE_COMMENT("Composes the mount trees that Manifold components are launched in."),

    SD_VARLINK_SYMBOL_COMMENT("The kind of a layer in a mount tree."),
    &vl_type_LayerKind,
    SD_VARLINK_SYMBOL_COMMENT("A layer of a mount tree."),
    &vl_type_Layer,

    SD_VARLINK_SYMBOL_COMMENT("Compose the mount tree for an application, or reuse one that was composed before. "
                              "May be called by any user."),
    &vl_method_PrepareLaunch,
    SD_VARLINK_SYMBOL_COMMENT("Discard the captured graphics stack of the host, e.g. after a driver update. "
                              "Privileged."),
    &vl_method_RefreshCapture,
    SD_VARLINK_SYMBOL_COMMENT("Enumerate the mount trees that are kept for reuse. Privileged."),
    &vl_method_ListTrees,

    SD_VARLINK_SYMBOL_COMMENT("The component is not installed."),
    &vl_error_NoSuchComponent,
    SD_VARLINK_SYMBOL_COMMENT("The component was revoked and must not be launched anymore."),
    &vl_error_ComponentRevoked,
    SD_VARLINK_SYMBOL_COMMENT("The mount tree for the component could not be composed."),
    &vl_error_CompositionFailed,
    SD_VARLINK_SYMBOL_COMMENT("There are no mount trees."),
    &vl_error_NoTrees);

/* clang-format on */
