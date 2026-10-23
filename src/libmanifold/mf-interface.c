/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define _GNU_SOURCE
#include "mf-interface-private.h"

/*
 * Definition of the org.freedesktop.manifold Varlink interface.
 */

/* clang-format off */

static SD_VARLINK_DEFINE_ENUM_TYPE(
    ComponentKind,
    SD_VARLINK_FIELD_COMMENT("The root filesystem everything else builds upon."),
    SD_VARLINK_DEFINE_ENUM_VALUE(core),
    SD_VARLINK_FIELD_COMMENT("A platform many applications share, based on a strict core version."),
    SD_VARLINK_DEFINE_ENUM_VALUE(platform),
    SD_VARLINK_FIELD_COMMENT("The packages/components an application needs beyond the layer below."),
    SD_VARLINK_DEFINE_ENUM_VALUE(kit),
    SD_VARLINK_FIELD_COMMENT("An application."),
    SD_VARLINK_DEFINE_ENUM_VALUE(app),
    SD_VARLINK_FIELD_COMMENT("A plugin of an application."),
    SD_VARLINK_DEFINE_ENUM_VALUE(plugin));

static SD_VARLINK_DEFINE_ENUM_TYPE(
    InstallMode,
    SD_VARLINK_FIELD_COMMENT("The component is kept until it is removed explicitly."),
    SD_VARLINK_DEFINE_ENUM_VALUE(manual),
    SD_VARLINK_FIELD_COMMENT("The component is removed once it is not needed anymore."),
    SD_VARLINK_DEFINE_ENUM_VALUE(auto));

static SD_VARLINK_DEFINE_STRUCT_TYPE(
    Component,
    SD_VARLINK_FIELD_COMMENT("Identifier of the component."),
    SD_VARLINK_DEFINE_FIELD(componentId, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("The kind of the component."),
    SD_VARLINK_DEFINE_FIELD_BY_TYPE(kind, ComponentKind, 0),
    SD_VARLINK_FIELD_COMMENT("Version of the component."),
    SD_VARLINK_DEFINE_FIELD(version, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("Revision of the version (Automatically set by the builder, or by a repository)."),
    SD_VARLINK_DEFINE_FIELD(revision, SD_VARLINK_INT, 0),
    SD_VARLINK_FIELD_COMMENT("Whether this is the SDK image of the component."),
    SD_VARLINK_DEFINE_FIELD(sdk, SD_VARLINK_BOOL, 0),
    SD_VARLINK_FIELD_COMMENT("The identity hash of the image, as a hex encoded string."),
    SD_VARLINK_DEFINE_FIELD(identity, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("The dm-verity root hash of the image, as a hex encoded string."),
    SD_VARLINK_DEFINE_FIELD(verityHash, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("Whether this is the active version of the component, the one that is launched."),
    SD_VARLINK_DEFINE_FIELD(current, SD_VARLINK_BOOL, 0),
    SD_VARLINK_FIELD_COMMENT("Whether the component is a potential target for automatic removal."),
    SD_VARLINK_DEFINE_FIELD_BY_TYPE(installMode, InstallMode, 0),
    SD_VARLINK_FIELD_COMMENT("Where the trust in the component came from: the name of a repository, or \"local\"."),
    SD_VARLINK_DEFINE_FIELD(origin, SD_VARLINK_STRING, 0));

static SD_VARLINK_DEFINE_ENUM_TYPE(
    Action,
    SD_VARLINK_FIELD_COMMENT("A component is installed."),
    SD_VARLINK_DEFINE_ENUM_VALUE(install),
    SD_VARLINK_FIELD_COMMENT("A component is removed on request."),
    SD_VARLINK_DEFINE_ENUM_VALUE(remove),
    SD_VARLINK_FIELD_COMMENT("An unused component is removed."),
    SD_VARLINK_DEFINE_ENUM_VALUE(cleanup));

static SD_VARLINK_DEFINE_STRUCT_TYPE(
    ItemProgress,
    SD_VARLINK_FIELD_COMMENT("What is being done to the subject."),
    SD_VARLINK_DEFINE_FIELD_BY_TYPE(action, Action, 0),
    SD_VARLINK_FIELD_COMMENT("Identifier of the component that is acted on."),
    SD_VARLINK_DEFINE_FIELD(subject, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("Version of the component, with its revision: '<version>-<rev>'."),
    SD_VARLINK_DEFINE_FIELD(version, SD_VARLINK_STRING, SD_VARLINK_NULLABLE),
    SD_VARLINK_FIELD_COMMENT("How far this item is, from 0 to 100."),
    SD_VARLINK_DEFINE_FIELD(percent, SD_VARLINK_FLOAT, 0),
    SD_VARLINK_FIELD_COMMENT("Bytes of the image copied so far, while an image is copied."),
    SD_VARLINK_DEFINE_FIELD(bytesDone, SD_VARLINK_INT, SD_VARLINK_NULLABLE),
    SD_VARLINK_FIELD_COMMENT("Size of the image in bytes, while an image is copied."),
    SD_VARLINK_DEFINE_FIELD(bytesTotal, SD_VARLINK_INT, SD_VARLINK_NULLABLE));

static SD_VARLINK_DEFINE_STRUCT_TYPE(
    Progress,
    SD_VARLINK_FIELD_COMMENT("How far the whole operation is, from 0 to 100."),
    SD_VARLINK_DEFINE_FIELD(percent, SD_VARLINK_FLOAT, 0),
    SD_VARLINK_FIELD_COMMENT("The item that is being worked on right now."),
    SD_VARLINK_DEFINE_FIELD_BY_TYPE(item, ItemProgress, SD_VARLINK_NULLABLE));

static SD_VARLINK_DEFINE_STRUCT_TYPE(
    Result,
    SD_VARLINK_FIELD_COMMENT("The components that were installed."),
    SD_VARLINK_DEFINE_FIELD_BY_TYPE(installed, Component, SD_VARLINK_ARRAY),
    SD_VARLINK_FIELD_COMMENT("The components that were removed, including dependencies nothing needed anymore."),
    SD_VARLINK_DEFINE_FIELD_BY_TYPE(removed, Component, SD_VARLINK_ARRAY),
    SD_VARLINK_FIELD_COMMENT("The root hashes of the images that were removed."),
    SD_VARLINK_DEFINE_FIELD(removedImages, SD_VARLINK_STRING, SD_VARLINK_ARRAY),
    SD_VARLINK_FIELD_COMMENT("Bytes of disk space the operation took up."),
    SD_VARLINK_DEFINE_FIELD(bytesAdded, SD_VARLINK_INT, 0),
    SD_VARLINK_FIELD_COMMENT("Bytes of disk space the operation freed."),
    SD_VARLINK_DEFINE_FIELD(bytesFreed, SD_VARLINK_INT, 0));

static SD_VARLINK_DEFINE_STRUCT_TYPE(
    Done,
    SD_VARLINK_FIELD_COMMENT("The components the operation changed, as they are now."),
    SD_VARLINK_DEFINE_FIELD_BY_TYPE(components, Component, SD_VARLINK_ARRAY));

static SD_VARLINK_DEFINE_ENUM_TYPE(
    TransactionFlag,
    SD_VARLINK_FIELD_COMMENT("The service may ask the user things, e.g. to authorize an operation."),
    SD_VARLINK_DEFINE_ENUM_VALUE(allowInteractive));

#define MF_TRANSACTION_FLAGS_INPUT \
    SD_VARLINK_FIELD_COMMENT("Flags that change how the operation is done."), \
    SD_VARLINK_DEFINE_INPUT_BY_TYPE(flags, TransactionFlag, SD_VARLINK_ARRAY | SD_VARLINK_NULLABLE)

#define MF_OPERATION_OUTPUT \
    SD_VARLINK_FIELD_COMMENT("How far the operation is. Only in the intermediate replies of a call with the 'more' flag."), \
    SD_VARLINK_DEFINE_OUTPUT_BY_TYPE(progress, Progress, SD_VARLINK_NULLABLE), \
    SD_VARLINK_FIELD_COMMENT("What the operation did. In the final reply."), \
    SD_VARLINK_DEFINE_OUTPUT_BY_TYPE(result, Result, SD_VARLINK_NULLABLE)

static SD_VARLINK_DEFINE_METHOD_FULL(
    InstallFile,
    SD_VARLINK_SUPPORTS_MORE,
    SD_VARLINK_FIELD_COMMENT("Index of the file descriptor of the bundle that should be installed."),
    SD_VARLINK_DEFINE_INPUT(bundleFileDescriptor, SD_VARLINK_INT, 0),
    MF_TRANSACTION_FLAGS_INPUT,
    MF_OPERATION_OUTPUT);

static SD_VARLINK_DEFINE_METHOD_FULL(
    RemoveComponent,
    SD_VARLINK_SUPPORTS_MORE,
    SD_VARLINK_FIELD_COMMENT("Identifier of the component to remove."),
    SD_VARLINK_DEFINE_INPUT(componentId, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("Version to remove, with all of its revisions, or '<version>-<rev>' to specify a revision explicitly. "
                             "All versions are removed if this is not set."),
    SD_VARLINK_DEFINE_INPUT(version, SD_VARLINK_STRING, SD_VARLINK_NULLABLE),
    MF_TRANSACTION_FLAGS_INPUT,
    MF_OPERATION_OUTPUT);

static SD_VARLINK_DEFINE_METHOD_FULL(
    Cleanup,
    SD_VARLINK_SUPPORTS_MORE,
    MF_TRANSACTION_FLAGS_INPUT,
    MF_OPERATION_OUTPUT);

static SD_VARLINK_DEFINE_METHOD(
    MarkComponent,
    SD_VARLINK_FIELD_COMMENT("Identifier of the component to mark."),
    SD_VARLINK_DEFINE_INPUT(componentId, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("Version to mark, with all of its revisions, or '<version>-<rev>' to specify a revision too. "
                             "The current version is marked if this is not set."),
    SD_VARLINK_DEFINE_INPUT(version, SD_VARLINK_STRING, SD_VARLINK_NULLABLE),
    SD_VARLINK_FIELD_COMMENT("The install mode to record."),
    SD_VARLINK_DEFINE_INPUT_BY_TYPE(mode, InstallMode, 0),
    MF_TRANSACTION_FLAGS_INPUT,
    SD_VARLINK_FIELD_COMMENT("The versions that were marked."),
    SD_VARLINK_DEFINE_OUTPUT_BY_TYPE(done, Done, 0));

static SD_VARLINK_DEFINE_ERROR(
    Failed,
    SD_VARLINK_FIELD_COMMENT("Description of what went wrong."),
    SD_VARLINK_DEFINE_FIELD(message, SD_VARLINK_STRING, 0));

static SD_VARLINK_DEFINE_ERROR(
    InvalidBundle,
    SD_VARLINK_FIELD_COMMENT("Description of what is wrong with the file."),
    SD_VARLINK_DEFINE_FIELD(message, SD_VARLINK_STRING, 0));

static SD_VARLINK_DEFINE_ERROR(
    VerificationFailed,
    SD_VARLINK_FIELD_COMMENT("Description of the problem."),
    SD_VARLINK_DEFINE_FIELD(message, SD_VARLINK_STRING, 0));

static SD_VARLINK_DEFINE_ERROR(
    MissingBase,
    SD_VARLINK_FIELD_COMMENT("Identifier of the component that is missing its base."),
    SD_VARLINK_DEFINE_FIELD(componentId, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("Identifier of the base that is missing."),
    SD_VARLINK_DEFINE_FIELD(baseId, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("Version of the base that is missing."),
    SD_VARLINK_DEFINE_FIELD(baseVersion, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("The identity hash an image would have to have to be the base."),
    SD_VARLINK_DEFINE_FIELD(identity, SD_VARLINK_STRING, 0));

static SD_VARLINK_DEFINE_ERROR(
    AlreadyInstalled,
    SD_VARLINK_FIELD_COMMENT("Identifier of the component that is installed already."),
    SD_VARLINK_DEFINE_FIELD(componentId, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("The version that is installed already."),
    SD_VARLINK_DEFINE_FIELD(version, SD_VARLINK_STRING, 0));

static SD_VARLINK_DEFINE_ERROR(
    NoSuchComponent,
    SD_VARLINK_FIELD_COMMENT("Identifier of the component that was not found."),
    SD_VARLINK_DEFINE_FIELD(componentId, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("The version that was not found, if only one version was asked for."),
    SD_VARLINK_DEFINE_FIELD(version, SD_VARLINK_STRING, SD_VARLINK_NULLABLE));

static SD_VARLINK_DEFINE_ERROR(
    ComponentInUse,
    SD_VARLINK_FIELD_COMMENT("Identifier of the component that is in use."),
    SD_VARLINK_DEFINE_FIELD(componentId, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("Identifier of an installed component that builds on it."),
    SD_VARLINK_DEFINE_FIELD(usedBy, SD_VARLINK_STRING, 0));

static SD_VARLINK_DEFINE_ERROR(
    WrongArchitecture,
    SD_VARLINK_FIELD_COMMENT("Identifier of the component that was built for another architecture."),
    SD_VARLINK_DEFINE_FIELD(componentId, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("The architecture the component was built for."),
    SD_VARLINK_DEFINE_FIELD(architecture, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("The architecture of this system."),
    SD_VARLINK_DEFINE_FIELD(hostArchitecture, SD_VARLINK_STRING, 0));

static SD_VARLINK_DEFINE_ERROR(
    IdConflict,
    SD_VARLINK_FIELD_COMMENT("Identifier of the component that can not be installed."),
    SD_VARLINK_DEFINE_FIELD(componentId, SD_VARLINK_STRING, 0),
    SD_VARLINK_FIELD_COMMENT("The kind of the component that is installed with this identifier."),
    SD_VARLINK_DEFINE_FIELD_BY_TYPE(kind, ComponentKind, 0),
    SD_VARLINK_FIELD_COMMENT("Whether the component that is installed with this identifier is an SDK."),
    SD_VARLINK_DEFINE_FIELD(sdk, SD_VARLINK_BOOL, 0));

SD_VARLINK_DEFINE_INTERFACE(
    org_freedesktop_manifold,
    MF_INTERFACE,
    SD_VARLINK_INTERFACE_COMMENT("Manages Manifold components."),

    SD_VARLINK_SYMBOL_COMMENT("The kind of a component."),
    &vl_type_ComponentKind,
    SD_VARLINK_SYMBOL_COMMENT("Whether a component is a potential target for automatic removal."),
    &vl_type_InstallMode,
    SD_VARLINK_SYMBOL_COMMENT("An installed component."),
    &vl_type_Component,
    SD_VARLINK_SYMBOL_COMMENT("What is done to a component or an image."),
    &vl_type_Action,
    SD_VARLINK_SYMBOL_COMMENT("How far one item of an operation is."),
    &vl_type_ItemProgress,
    SD_VARLINK_SYMBOL_COMMENT("How far an operation is."),
    &vl_type_Progress,
    SD_VARLINK_SYMBOL_COMMENT("What an operation did."),
    &vl_type_Result,
    SD_VARLINK_SYMBOL_COMMENT("What an operation did that installs and removes nothing."),
    &vl_type_Done,
    SD_VARLINK_SYMBOL_COMMENT("A general flag that changes how operations are performed."),
    &vl_type_TransactionFlag,

    SD_VARLINK_SYMBOL_COMMENT("Install the components of a bundle, which is passed as a file descriptor. "
                              "Called with the 'more' flag, reports its progress. Privileged."),
    &vl_method_InstallFile,
    SD_VARLINK_SYMBOL_COMMENT("Remove a component, or one version of it. "
                              "Called with the 'more' flag, reports its progress. Privileged."),
    &vl_method_RemoveComponent,
    SD_VARLINK_SYMBOL_COMMENT("Remove unneeded cruft: Dependencies no installed component builds on, "
                              "images no component links to, and what interrupted installations left. "
                              "Called with the 'more' flag, reports its progress. Privileged."),
    &vl_method_Cleanup,
    SD_VARLINK_SYMBOL_COMMENT("Mark a version of a component as kept until it is removed explicitly (manual), "
                              "or as removed once it is not needed anymore (auto). Nothing is removed "
                              "right away. Privileged."),
    &vl_method_MarkComponent,

    SD_VARLINK_SYMBOL_COMMENT("The operation failed."),
    &vl_error_Failed,
    SD_VARLINK_SYMBOL_COMMENT("The file is not a Manifold bundle."),
    &vl_error_InvalidBundle,
    SD_VARLINK_SYMBOL_COMMENT("The contents of the bundle do not match what its index and manifests say."),
    &vl_error_VerificationFailed,
    SD_VARLINK_SYMBOL_COMMENT("The base a component builds on is not installed."),
    &vl_error_MissingBase,
    SD_VARLINK_SYMBOL_COMMENT("This version of the component is installed already."),
    &vl_error_AlreadyInstalled,
    SD_VARLINK_SYMBOL_COMMENT("The component is not installed."),
    &vl_error_NoSuchComponent,
    SD_VARLINK_SYMBOL_COMMENT("An installed component builds on this component."),
    &vl_error_ComponentInUse,
    SD_VARLINK_SYMBOL_COMMENT("The component architecture and that of the current system are incompatible."),
    &vl_error_WrongArchitecture,
    SD_VARLINK_SYMBOL_COMMENT("Another kind of component is installed with the identifier of this component."),
    &vl_error_IdConflict);

/* clang-format on */
