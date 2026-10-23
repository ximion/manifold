/*
 * Copyright (C) 2020-2026 Collabora Ltd.
 * Copyright (C) 2024-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define _GNU_SOURCE
#include "config.h"
#include "mfr-capture.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <libglnx.h>
#include <systemd/sd-json.h>

#include "mf-sdautoptr-private.h"
#include "mf-json-private.h"
#include "mf-utils-private.h"
#include "mfr-tree.h"

/*
 * The graphics stack of the host, captured for a Manifold core.
 *
 * capsule-capture-libs is run with the host as provider and the mounted core
 * as container. It builds a farm of symbolic links to the libraries that have
 * to come from the host: the graphics drivers and their dependencies, and
 * glibc and libstdc++ where the host's are newer. The links point below
 * /run/host, where the host is bound in every mount tree. The loader
 * manifests of the host (Vulkan, EGL, OpenCL) are rewritten to point at the
 * captured drivers, and the environment variables that make the loaders use
 * them are recorded.
 *
 * A lot of code, design patterns and library family data are taken from
 * pressure-vessel (steam-runtime-tools, runtime.c) by Valve/Collabora.
 * That code is a great reference as well, and we should keep our version
 * in sync with Valve's where sensible.
 *
 * A capture is a directory below MF_RUN_CACHE_DIR/capture/<core-hash>/:
 *
 *   overrides/          the layer that goes on top of the images
 *   environment.json    the loader variables, "" for one that must be unset
 *   stamp               what on the host the capture was made from
 *
 * An #MfrCapture describes the capture for one core image and is loaded
 * from its directory, or built, by mfr_capture_ensure(). A capture
 * is stale when the host libraries it links to changed, or when loader
 * manifests were added, removed or replaced. Every link is checked
 * when the daemon starts, before a regular app launch, only the mtimes
 * of the library and manifest directories are compared.
 *
 * A capture that we do not use anymore is moved aside while the daemon runs,
 * and deleted when the daemon starts, see mfr_capture_move_aside().
 */

/* where the captured vendor drivers and the rewritten loader manifests go in a tree, kept apart
 * from the image's own */
constexpr char GRAPHICS_OVERRIDES[] = "/usr/lib/manifold/graphics";

constexpr char OVERRIDES_NAME[] = "overrides";
constexpr char ENVIRONMENT_NAME[] = "environment.json";
constexpr char STAMP_NAME[] = "stamp";

constexpr char STALE_PREFIX[] = ".stale-";

/**
 * MfrDriverKind:
 *
 * A kind of dlopen()ed vendor driver: how its files are named, the variables
 * that point its loaders at the drivers, and where on the host they are.
 * Distributions put the drivers in different places, but always in a
 * subdirectory of the directory that holds the library loading them
 * (Slackware has the DRI drivers in xorg/modules/dri).
 */
typedef struct {
    const char *subdir; /* where we capture the drivers to, below GRAPHICS_OVERRIDES/<arch_tuple> */
    const char *prefix;
    const char *const suffixes[3];
    const char *const variables[3];
    const char *const loaders[6];
    const char *const lookups[3]; /* subdirectories of a loader's directory */
} MfrDriverKind;

/* clang-format off */
static const MfrDriverKind DRIVER_KINDS[] = {
    {"dri",   "",          {"_dri.so", "_drv_video.so"}, {"LIBGL_DRIVERS_PATH", "LIBVA_DRIVERS_PATH"},
     {"libGLX_mesa.so.0", "libEGL_mesa.so.0", "libGL.so.1", "libva.so.2", "libva.so.1"},
     {"dri", "xorg/modules/dri"}},
    {"gbm",   "",          {"_gbm.so"},                  {"GBM_BACKENDS_PATH"},
     {"libgbm.so.1"},
     {"gbm"}},
    {"vdpau", "libvdpau_", {".so.1"},                    {"VDPAU_DRIVER_PATH"},
     {"libvdpau.so.1"},
     {"vdpau"}},
};
/* clang-format on */

/* captured only if the host's copy is newer than the image's */
static const char *const SONAMES[] = {
    "libvulkan.so.1",
    "libvdpau.so.1",
    "libva.so.1",
    "libva-drm.so.1",
    "libva-glx.so.1",
    "libva-x11.so.1",
    "libva.so.2",
    "libva-drm.so.2",
    "libva-glx.so.2",
    "libva-x11.so.2",
    "libdrm.so.2",
    "libdrm_amdgpu.so.1",
    "libdrm_etnaviv.so.1",
    "libdrm_freedreno.so.1",
    "libdrm_intel.so.1",
    "libdrm_nouveau.so.2",
    "libdrm_radeon.so.1",
    "libdrm_tegra.so.0",
};

/* always taken from the host: these are the entry points into its kernel driver */
static const char *const SONAMES_EVEN_IF_OLDER[] = {
    "libEGL.so.1",
    "libGL.so.1",
    "libGLESv1_CM.so.1",
    "libGLESv2.so.2",
    "libGLX.so.0",
    "libGLX_indirect.so.0",
    "libGLdispatch.so.0",
    "libOpenCL.so.1",
    "libOpenGL.so.0",
    "libEGL_mesa.so.0",
    "libGLX_mesa.so.0",
    "libEGL_nvidia.so.0",
    "libGLESv1_CM_nvidia.so.1",
    "libGLESv2_nvidia.so.2",
    "libGLX_nvidia.so.0",
    "libXNVCtrl.so.0",
    "libcuda.so.1",
    "libcudadebugger.so.1",
    "libnvcuvid.so.1",
    "libnvidia-allocator.so.1",
    "libnvidia-api.so.1",
    "libnvidia-cfg.so.1",
    "libnvidia-egl-gbm.so.1",
    "libnvidia-egl-wayland.so.1",
    "libnvidia-egl-wayland2.so.1",
    "libnvidia-egl-xcb.so.1",
    "libnvidia-egl-xlib.so.1",
    "libnvidia-encode.so.1",
    "libnvidia-fbc.so.1",
    "libnvidia-ifr.so.1",
    "libnvidia-ml.so.1",
    "libnvidia-ngx.so.1",
    "libnvidia-nvvm.so.4",
    "libnvidia-nvvm70.so.4",
    "libnvidia-opencl.so.1",
    "libnvidia-opticalflow.so.1",
    "libnvidia-ptxjitcompiler.so.1",
    "libnvoptix.so.1",
    "libvdpau_nvidia.so.1",
};

/*
 * Libraries matching these patterns are taken from the host even if older
 * than the image's: applications look up symbols in all of them with dlsym().
 * The NVIDIA names are listed one by one, as the vendor also ships
 * libnvidia-gtk*, libnvidia-wayland-client and the like, which would pull
 * toolkits in.
 */
static const char *const SONAME_GLOBS_EVEN_IF_OLDER[] = {
    "libEGL.so.*",
    "libGL.so.*",
    "libGLESv1_CM.so.*",
    "libGLESv2.so.*",
    "libGLX.so.*",
    "libGLX_indirect.so.*",
    "libGLdispatch.so.*",
    "libOpenCL.so.*",
    "libOpenGL.so.*",
    "libEGL_nvidia.so.*",
    "libGLESv1_CM_nvidia.so.*",
    "libGLESv2_nvidia.so.*",
    "libGLX_nvidia.so.*",
    "libXNVCtrl.so.*",
    "libcuda.so.*",
    "libcudadebugger.so.*",
    "libglx.so.*",
    "libnvcuvid.so.*",
    "libnvidia-allocator.so.*",
    "libnvidia-api.so.*",
    "libnvidia-cbl.so.*",
    "libnvidia-cfg.so.*",
    "libnvidia-compiler.so.*",
    "libnvidia-egl-*.so.*",
    "libnvidia-eglcore.so.*",
    "libnvidia-encode.so.*",
    "libnvidia-fatbinaryloader.so.*",
    "libnvidia-fbc.so.*",
    "libnvidia-glcore.so.*",
    "libnvidia-glsi.so.*",
    "libnvidia-glvkspirv.so.*",
    "libnvidia-gpucomp.so.*",
    "libnvidia-ifr.so.*",
    "libnvidia-ml.so.*",
    "libnvidia-ngx.so.*",
    "libnvidia-nvvm*.so.*",
    "libnvidia-opencl.so.*",
    "libnvidia-opticalflow.so.*",
    "libnvidia-present.so.*",
    "libnvidia-ptxjitcompiler.so.*",
    "libnvidia-rtcore.so.*",
    "libnvidia-tileiras.so.*",
    "libnvidia-tls.so.*",
    "libnvidia-vulkan-producer.so.*",
    "libnvoptix.so.*",
    "libvdpau_nvidia.so.*",
};

/* NVIDIA driver internals have no stable SONAME, they carry the driver version */
static const char *const NVIDIA_PRIVATE[] = {
    "eglcore",
    "glcore",
    "glsi",
    "glvkspirv",
    "gpucomp",
    "rtcore",
    "tls",
    "vulkan-producer",
};

/* so that a compatibility link like libudev.so.0 -> libudev.so.1 does not shadow the real thing */
static const char *const EXACT_SONAMES[] = {
    "libsystemd.so.0",
    "libudev.so.1",
    "libudev.so.0",
};

/*
 * Libraries whose companions must come from the same source: if the main
 * library was taken from the host, its relatives are taken too. The NSS
 * modules are loaded by glibc and must match it; libssl must match
 * libcrypto.
 */
static const struct {
    const char *soname;
    const char *const relatives[10];
} LIBRARY_FAMILIES[] = {
    {"libc.so.6",
     {"if-exists:libidn2.so.0",
      "if-exists:even-if-older:soname:libnss_compat.so.2",
      "if-exists:even-if-older:soname-match:libnss_compat.so.*",
      "if-exists:even-if-older:soname:libnss_db.so.2",
      "if-exists:even-if-older:soname-match:libnss_db.so.*",
      "if-exists:even-if-older:soname:libnss_dns.so.2",
      "if-exists:even-if-older:soname-match:libnss_dns.so.*",
      "if-exists:even-if-older:soname:libnss_files.so.2",
      "if-exists:even-if-older:soname-match:libnss_files.so.*"}     },
    {"libcrypto.so.3",    {"if-exists:soname:libssl.so.3"}          },
    {"libxkbcommon.so.0", {"if-exists:soname:libxkbcommon-x11.so.0"}},
};

/* data directories a library finds by its install prefix, taken from the host with the library */
static const struct {
    const char *soname;
    const char *dir; /* below usr/share */
} LIBRARY_DATA_DIRS[] = {
    {"libdrm_amdgpu.so.1", "libdrm" },
    {"libdrm.so.2",        "libdrm" },
    {"libGLX_mesa.so.0",   "drirc.d"},
    {"libEGL_mesa.so.0",   "drirc.d"},
    {"libGLX_nvidia.so.0", "nvidia" },
};

/* the programs of glibc, which can fail against a libc.so.6 they were not built with */
static const char *const GLIBC_EXECUTABLES[] = {
    "usr/sbin/ldconfig",
    "usr/bin/catchsegv",
    "usr/bin/getconf",
    "usr/bin/getent",
    "usr/bin/iconv",
    "usr/bin/ldd",
    "usr/bin/locale",
    "usr/bin/localedef",
    "usr/bin/pldd",
    "usr/bin/tzselect",
    "usr/bin/zdump",
};

/**
 * MfrLoaderManifestKind:
 *
 * One family of loader manifests: where the host keeps them, where the
 * rewritten copies go, and which environment variables point at them.
 */
typedef struct {
    const char *label;
    const char *const dirs[4]; /* host directories, highest priority first */
    const char *suffix;
    const char *dest;               /* below GRAPHICS_OVERRIDES/<arch_tuple or common> */
    const char *libsubdir;          /* below the libdir, for drivers named by path */
    const char *const env_files[3]; /* take a list of manifest files */
    const char *const env_dirs[2];  /* take a list of directories */
    const char *const env_unset[2]; /* must not compete with the above */
    gboolean is_json;
    gboolean per_arch;
} MfrLoaderManifestKind;

static const MfrLoaderManifestKind MFR_LOADER_MANIFEST_KINDS[] = {
    {
     .label = "Vulkan ICD",
     .dirs = {"/etc/vulkan/icd.d", "/usr/local/share/vulkan/icd.d", "/usr/share/vulkan/icd.d"},
     .suffix = ".json",
     .dest = "share/vulkan/icd.d",
     .libsubdir = "vulkan",
     /* VK_ICD_FILENAMES is deprecated, but older loaders only know it */
        .env_files = {"VK_DRIVER_FILES", "VK_ICD_FILENAMES"},
     .env_unset = {"VK_ADD_DRIVER_FILES"},
     .is_json = TRUE,
     .per_arch = TRUE,
     },
    {
     .label = "Vulkan implicit layer",
     .dirs = {"/etc/vulkan/implicit_layer.d",
                 "/usr/local/share/vulkan/implicit_layer.d",
                 "/usr/share/vulkan/implicit_layer.d"},
     .suffix = ".json",
     .dest = "share/vulkan/implicit_layer.d",
     .libsubdir = "vulkan_layer",
     .env_dirs = {"VK_IMPLICIT_LAYER_PATH"},
     .env_unset = {"VK_ADD_IMPLICIT_LAYER_PATH"},
     .is_json = TRUE,
     .per_arch = TRUE,
     },
    {
     .label = "Vulkan explicit layer",
     .dirs = {"/etc/vulkan/explicit_layer.d",
                 "/usr/local/share/vulkan/explicit_layer.d",
                 "/usr/share/vulkan/explicit_layer.d"},
     .suffix = ".json",
     .dest = "share/vulkan/explicit_layer.d",
     .libsubdir = "vulkan_layer",
     .env_dirs = {"VK_LAYER_PATH"},
     .env_unset = {"VK_ADD_LAYER_PATH"},
     .is_json = TRUE,
     .per_arch = TRUE,
     },
    {
     .label = "EGL vendor ICD",
     .dirs = {"/etc/glvnd/egl_vendor.d", "/usr/share/glvnd/egl_vendor.d"},
     .suffix = ".json",
     .dest = "share/glvnd/egl_vendor.d",
     .libsubdir = "glvnd",
     .env_files = {"__EGL_VENDOR_LIBRARY_FILENAMES"},
     .env_unset = {"__EGL_VENDOR_LIBRARY_DIRS"},
     .is_json = TRUE,
     .per_arch = TRUE,
     },
    {
     .label = "EGL external platform",
     .dirs = {"/etc/egl/egl_external_platform.d", "/usr/share/egl/egl_external_platform.d"},
     .suffix = ".json",
     .dest = "share/egl/egl_external_platform.d",
     .libsubdir = "glvnd",
     .env_files = {"__EGL_EXTERNAL_PLATFORM_CONFIG_FILENAMES"},
     .env_unset = {"__EGL_EXTERNAL_PLATFORM_CONFIG_DIRS"},
     .is_json = TRUE,
     .per_arch = TRUE,
     },
    {
     .label = "OpenCL ICD",
     .dirs = {"/etc/OpenCL/vendors"},
     .suffix = ".icd",
     .dest = "etc/OpenCL/vendors",
     .libsubdir = "opencl",
     /* ocl-icd takes one directory for all architectures; SONAMEs in .icd
      * files are left alone, ld.so.cache resolves them for the asking ABI */
        .env_dirs = {"OCL_ICD_VENDORS"},
     .is_json = FALSE,
     .per_arch = FALSE,
     },
};

/**
 * MfrLoaderManifestPlan:
 *
 * One manifest of the host, and what has to be captured for it.
 */
typedef struct {
    const MfrLoaderManifestKind *kind;
    char *source;
    sd_json_variant *manifest; /* the parsed manifest, NULL for an OpenCL .icd file */
    GPtrArray *sonames;        /* to capture into the libdir */
    GPtrArray *absolute;       /* host paths to capture into libdir/<libsubdir>/ */
} MfrLoaderManifestPlan;

/**
 * MfrLoaderManifestGroup:
 *
 * The manifests of the host of one kind.
 */
typedef struct {
    const MfrLoaderManifestKind *kind;
    GPtrArray *plans; /* of MfrLoaderManifestPlan */
} MfrLoaderManifestGroup;

struct _MfrCapture {
    GObject parent_instance;

    char *dir;
    char *overrides_dir;
    char *arch_tuple;   /* NULL for a capture found by its directory alone */
    char *id;           /* unique ID of the capture, NULL until the capture is taken */
    char **environment; /* VAR=VALUE, with VALUE empty for one to unset */
    sd_json_variant *stamp;
};

G_DEFINE_FINAL_TYPE(MfrCapture, mfr_capture, G_TYPE_OBJECT)

/**
 * mfr_capture_init:
 */
static void
mfr_capture_init(MfrCapture *capture)
{
}

/**
 * mfr_capture_finalize:
 */
static void
mfr_capture_finalize(GObject *object)
{
    MfrCapture *capture = MFR_CAPTURE(object);

    g_free(capture->dir);
    g_free(capture->overrides_dir);
    g_free(capture->arch_tuple);
    g_free(capture->id);
    g_strfreev(capture->environment);
    sd_json_variant_unref(capture->stamp);

    G_OBJECT_CLASS(mfr_capture_parent_class)->finalize(object);
}

/**
 * mfr_capture_class_init:
 */
static void
mfr_capture_class_init(MfrCaptureClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = mfr_capture_finalize;
}

/**
 * MfrDriverList:
 *
 * The drivers of one kind found on the host. A driver found in a directory
 * searched later does not replace one of the same name found before.
 */
typedef struct {
    const MfrDriverKind *kind;
    GPtrArray *paths;  /* host paths, in the order found */
    GHashTable *names; /* their base names, as a set */
} MfrDriverList;

/**
 * mfr_driver_list_init:
 */
static void
mfr_driver_list_init(MfrDriverList *list, const MfrDriverKind *kind)
{
    list->kind = kind;
    list->paths = g_ptr_array_new_with_free_func(g_free);
    list->names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}

/**
 * mfr_driver_list_clear:
 */
static void
mfr_driver_list_clear(MfrDriverList *list)
{
    g_clear_pointer(&list->paths, g_ptr_array_unref);
    g_clear_pointer(&list->names, g_hash_table_unref);
}

/**
 * mfr_driver_list_add:
 *
 * Add the driver @name in the host directory @dir, unless one of that name
 * was found already.
 */
static void
mfr_driver_list_add(MfrDriverList *list, const char *dir, const char *name)
{
    if (!g_hash_table_add(list->names, g_strdup(name)))
        return;
    g_ptr_array_add(list->paths, g_build_filename(dir, name, NULL));
}

/**
 * MfrCaptureJob:
 *
 * What we need to know and keep track of while we make a capture.
 */
typedef struct {
    const char *tool;
    const char *core_mount;
    const char *arch_tuple;
    char *overrides_dir;
    char *libdir;           /* overrides_dir/usr/lib/<arch_tuple> */
    char *tree_libdir;      /* /usr/lib/<arch_tuple> */
    char *drivers_dir;      /* overrides_dir/GRAPHICS_OVERRIDES/<arch_tuple> */
    char *tree_drivers_dir; /* GRAPHICS_OVERRIDES/<arch_tuple> */
    GHashTable *env;        /* variable -> GPtrArray of values */
    GHashTable *placed;     /* host path of a driver -> path in the tree */
    /* the drivers of each kind */
    MfrDriverList drivers[G_N_ELEMENTS(DRIVER_KINDS)];
    /* the host directories the drivers are looked for in */
    GPtrArray *host_dirs;
} MfrCaptureJob;

/**
 * mfr_loader_manifest_plan_free:
 */
static void
mfr_loader_manifest_plan_free(MfrLoaderManifestPlan *plan)
{
    g_free(plan->source);
    sd_json_variant_unref(plan->manifest);
    g_ptr_array_unref(plan->sonames);
    g_ptr_array_unref(plan->absolute);
    g_free(plan);
}

/**
 * mfr_loader_manifest_group_free:
 */
static void
mfr_loader_manifest_group_free(MfrLoaderManifestGroup *group)
{
    g_ptr_array_unref(group->plans);
    g_free(group);
}

/**
 * mfr_env_table_add:
 */
static void
mfr_env_table_add(GHashTable *env, const char *variable, const char *value)
{
    GPtrArray *values = g_hash_table_lookup(env, variable);

    if (values == NULL) {
        values = g_ptr_array_new_with_free_func(g_free);
        g_hash_table_insert(env, g_strdup(variable), values);
    }
    for (guint i = 0; i < values->len; i++) {
        if (g_str_equal(g_ptr_array_index(values, i), value))
            return;
    }
    g_ptr_array_add(values, g_strdup(value));
}

/**
 * mfr_capture_get_managed_variables:
 *
 * Every variable the loaders might read. The ones we have no value for are
 * unset in the application, as a value from the host's session would point at a
 * path that does not exist in the tree.
 */
static char **
mfr_capture_get_managed_variables(void)
{
    g_autoptr(GPtrArray) names = g_ptr_array_new();

    for (size_t i = 0; i < G_N_ELEMENTS(MFR_LOADER_MANIFEST_KINDS); i++) {
        const MfrLoaderManifestKind *kind = &MFR_LOADER_MANIFEST_KINDS[i];

        for (size_t j = 0; kind->env_files[j] != NULL; j++)
            g_ptr_array_add(names, (char *) kind->env_files[j]);
        for (size_t j = 0; kind->env_dirs[j] != NULL; j++)
            g_ptr_array_add(names, (char *) kind->env_dirs[j]);
        for (size_t j = 0; kind->env_unset[j] != NULL; j++)
            g_ptr_array_add(names, (char *) kind->env_unset[j]);
    }
    for (size_t i = 0; i < G_N_ELEMENTS(DRIVER_KINDS); i++) {
        for (size_t j = 0; DRIVER_KINDS[i].variables[j] != NULL; j++)
            g_ptr_array_add(names, (char *) DRIVER_KINDS[i].variables[j]);
    }
    g_ptr_array_sort_values(names, (GCompareFunc) g_strcmp0);
    g_ptr_array_add(names, NULL);

    return g_strdupv((char **) names->pdata);
}

/**
 * mfr_capture_run_tool:
 *
 * Run capsule-capture-libs, or anything else, and fail if it does.
 */
static gboolean
mfr_capture_run_tool(GPtrArray *argv, char **ret_stdout, GError **error)
{
    g_autofree char *standard_output = NULL;
    g_autofree char *standard_error = NULL;
    g_autofree char *command = NULL;
    int wait_status;

    g_ptr_array_add(argv, NULL);
    command = g_strjoinv(" ", (char **) argv->pdata);
    g_debug("Running: %s", command);

    if (!g_spawn_sync(NULL,
                      (char **) argv->pdata,
                      NULL,
                      G_SPAWN_DEFAULT,
                      NULL,
                      NULL,
                      &standard_output,
                      &standard_error,
                      &wait_status,
                      error))
        return FALSE;
    if (!g_spawn_check_wait_status(wait_status, error)) {
        g_strchomp(standard_error);
        return glnx_prefix_error(error,
                                 "%s failed (%s)",
                                 (char *) g_ptr_array_index(argv, 0),
                                 standard_error);
    }

    if (ret_stdout != NULL)
        *ret_stdout = g_steal_pointer(&standard_output);
    return TRUE;
}

/**
 * mfr_capture_job_make_argv:
 *
 * Returns: (transfer full): a newly allocated array of strings
 */
static GPtrArray *
mfr_capture_job_make_argv(MfrCaptureJob *job, const char *dest)
{
    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);

    g_ptr_array_add(argv, g_strdup(job->tool));
    g_ptr_array_add(argv, g_strdup("--provider=/"));
    /* comparing against the core implements "newest of host and image" */
    g_ptr_array_add(argv, g_strdup_printf("--container=%s", job->core_mount));
    /* the host is at /run/host in the tree, so every link has to point there */
    g_ptr_array_add(argv, g_strdup_printf("--link-target=%s", MFR_HOST_DIR));
    g_ptr_array_add(argv, g_strdup_printf("--dest=%s", dest));

    return argv;
}

/**
 * mfr_read_nvidia_version:
 *
 * Returns: (transfer full): a newly allocated string
 */
static char *
mfr_read_nvidia_version(void)
{
    g_autofree char *version = NULL;

    if (!g_file_get_contents("/sys/module/nvidia/version", &version, NULL, NULL))
        return NULL;
    return g_strchomp(g_steal_pointer(&version));
}

/**
 * mfr_capture_job_add_drivers_in:
 *
 * Add the drivers of the kind of @list in a host directory to @list.
 */
static void
mfr_capture_job_add_drivers_in(MfrCaptureJob *job, MfrDriverList *list, const char *dir)
{
    g_autoptr(GDir) entries = g_dir_open(dir, 0, NULL);
    g_autoptr(GPtrArray) names = g_ptr_array_new();
    const char *name;

    if (!g_ptr_array_find_with_equal_func(job->host_dirs, dir, g_str_equal, NULL))
        g_ptr_array_add(job->host_dirs, g_strdup(dir));

    while (entries != NULL && (name = g_dir_read_name(entries)) != NULL) {
        gboolean wanted = FALSE;

        if (!g_str_has_prefix(name, list->kind->prefix))
            continue;
        for (size_t i = 0; list->kind->suffixes[i] != NULL; i++)
            wanted |= g_str_has_suffix(name, list->kind->suffixes[i]);
        /* nvidia-vaapi-driver depends on GStreamer, which is more than a capture can carry */
        if (wanted && !g_str_equal(name, "nvidia_drv_video.so"))
            g_ptr_array_add(names, (char *) name);
    }
    g_ptr_array_sort_values(names, (GCompareFunc) g_strcmp0);

    for (guint i = 0; i < names->len; i++)
        mfr_driver_list_add(list, dir, g_ptr_array_index(names, i));
}

/**
 * mfr_capture_job_find_drivers:
 *
 * Find the dlopen()ed vendor drivers of the host: the capture tool looks
 * up the libraries that load them in the host's ld.so.cache, and the
 * drivers are in subdirectories of the directories those libraries are
 * in, whatever the distribution calls them.
 */
static gboolean
mfr_capture_job_find_drivers(MfrCaptureJob *job, GError **error)
{
    g_auto(GLnxTmpDir) links_dir = {0};
    g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);

    if (!glnx_mkdtemp("manifold-loaders-XXXXXX", 0700, &links_dir, error))
        return FALSE;
    g_ptr_array_add(argv, g_strdup(job->tool));
    g_ptr_array_add(argv, g_strdup_printf("--dest=%s", links_dir.path));
    g_ptr_array_add(argv, g_strdup("--provider=/"));
    /* links to where the loaders are on the host, not to where the host is in a tree */
    g_ptr_array_add(argv, g_strdup("--link-target=/"));
    for (size_t i = 0; i < G_N_ELEMENTS(DRIVER_KINDS); i++) {
        for (size_t j = 0; DRIVER_KINDS[i].loaders[j] != NULL; j++)
            g_ptr_array_add(argv,
                            g_strdup_printf("no-dependencies:if-exists:even-if-older:soname:%s",
                                            DRIVER_KINDS[i].loaders[j]));
    }
    if (!mfr_capture_run_tool(argv, NULL, error))
        return FALSE;

    for (size_t i = 0; i < G_N_ELEMENTS(job->drivers); i++) {
        MfrDriverList *list = &job->drivers[i];
        g_autoptr(GHashTable) libdirs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

        for (size_t j = 0; list->kind->loaders[j] != NULL; j++) {
            g_autofree char *link = g_build_filename(links_dir.path, list->kind->loaders[j], NULL);
            g_autofree char *loader = g_file_read_link(link, NULL);
            g_autofree char *libdir = NULL;
            g_autofree char *canonical = NULL;

            if (loader == NULL)
                continue;
            libdir = g_path_get_dirname(loader);
            canonical = realpath(libdir, NULL);
            if (canonical != NULL)
                g_set_str(&libdir, canonical);
            /* the loaders of a kind are usually in the same directory, which we scan only once */
            if (!g_hash_table_add(libdirs, g_strdup(libdir)))
                continue;
            if (!g_ptr_array_find_with_equal_func(job->host_dirs, libdir, g_str_equal, NULL))
                g_ptr_array_add(job->host_dirs, g_strdup(libdir));
            for (size_t k = 0; list->kind->lookups[k] != NULL; k++) {
                g_autofree char *dir = g_build_filename(libdir, list->kind->lookups[k], NULL);

                mfr_capture_job_add_drivers_in(job, list, dir);
            }
        }
    }

    return TRUE;
}

/**
 * mfr_capture_job_register_patterns:
 *
 * Register the patterns for the main run of the capture tool (everything but
 * the vendor drivers themselves, which go into subdirectories of their own).
 */
static void
mfr_capture_job_register_patterns(MfrCaptureJob *job, GPtrArray *argv)
{
    g_autofree char *nvidia = mfr_read_nvidia_version();

    /* the built-in shortcut for the Mesa/GLVND core libraries */
    g_ptr_array_add(argv, g_strdup("gl:"));
    for (size_t i = 0; i < G_N_ELEMENTS(SONAMES); i++)
        g_ptr_array_add(argv, g_strdup_printf("if-exists:if-same-abi:soname:%s", SONAMES[i]));
    for (size_t i = 0; i < G_N_ELEMENTS(SONAMES_EVEN_IF_OLDER); i++)
        g_ptr_array_add(
            argv,
            g_strdup_printf("if-exists:even-if-older:if-same-abi:soname:%s", SONAMES_EVEN_IF_OLDER[i]));
    for (size_t i = 0; i < G_N_ELEMENTS(SONAME_GLOBS_EVEN_IF_OLDER); i++)
        g_ptr_array_add(
            argv,
            g_strdup_printf("if-exists:even-if-older:soname-match:%s", SONAME_GLOBS_EVEN_IF_OLDER[i]));
    for (size_t i = 0; i < G_N_ELEMENTS(EXACT_SONAMES); i++)
        g_ptr_array_add(argv, g_strdup_printf("if-exists:if-same-abi:exact-soname:%s", EXACT_SONAMES[i]));

    if (nvidia != NULL) {
        g_info("Host NVIDIA driver %s detected", nvidia);
        for (size_t i = 0; i < G_N_ELEMENTS(NVIDIA_PRIVATE); i++)
            g_ptr_array_add(argv,
                            g_strdup_printf("if-exists:even-if-older:soname:libnvidia-%s.so.%s",
                                            NVIDIA_PRIVATE[i],
                                            nvidia));
    }

    /* the dependencies of the vendor drivers; the drivers come separately */
    for (size_t i = 0; i < G_N_ELEMENTS(job->drivers); i++) {
        for (guint j = 0; j < job->drivers[i].paths->len; j++)
            g_ptr_array_add(argv,
                            g_strdup_printf("only-dependencies:if-exists:if-same-abi:path:%s",
                                            (char *) g_ptr_array_index(job->drivers[i].paths, j)));
    }
}

/**
 * mfr_capture_job_capture_drivers:
 *
 * Capture the dlopen()ed vendor drivers of the host (DRI, VA-API, GBM and
 * VDPAU) into subdirectories below GRAPHICS_OVERRIDES, named as on the host.
 * Their loaders do not look up SONAMEs, but scan one directory each, which the
 * variables of DRIVER_KINDS point them at.
 *
 * We keep the drivers apart from the image's own driver directories, which
 * the loaders then never see. Otherwise the overlay would merge both, and
 * drivers of two Mesa versions could be loaded side by side.
 *
 * The capture tool takes a single destination, so this runs it once per
 * subdirectory. The dependencies of the drivers were captured into the libdir
 * by the main run already, so only the drivers themselves are taken here, and
 * always the host's, even if the core has newer ones.
 */
static gboolean
mfr_capture_job_capture_drivers(MfrCaptureJob *job, GError **error)
{
    for (size_t i = 0; i < G_N_ELEMENTS(job->drivers); i++) {
        GPtrArray *drivers = job->drivers[i].paths;
        g_autofree char *dest = g_build_filename(job->drivers_dir, job->drivers[i].kind->subdir, NULL);
        g_autoptr(GPtrArray) argv = NULL;

        if (drivers->len == 0)
            continue;
        if (!glnx_shutil_mkdir_p_at(AT_FDCWD, dest, 0755, NULL, error))
            return FALSE;

        argv = mfr_capture_job_make_argv(job, dest);
        for (guint j = 0; j < drivers->len; j++)
            g_ptr_array_add(argv,
                            g_strdup_printf("no-dependencies:even-if-older:if-exists:if-same-abi:path:%s",
                                            (char *) g_ptr_array_index(drivers, j)));
        if (!mfr_capture_run_tool(argv, NULL, error))
            return FALSE;
    }

    return TRUE;
}

/**
 * mfr_capture_job_check_captured:
 *
 * Whether the main run took a library from the host.
 */
static gboolean
mfr_capture_job_check_captured(MfrCaptureJob *job, const char *soname)
{
    g_autofree char *link = g_build_filename(job->libdir, soname, NULL);

    return g_file_test(link, G_FILE_TEST_IS_SYMLINK);
}

/**
 * mfr_capture_job_capture_families:
 *
 * Capture the relatives of each LIBRARY_FAMILIES library that the main run
 * took from the host, so they come from the same source as it.
 */
static gboolean
mfr_capture_job_capture_families(MfrCaptureJob *job, GError **error)
{
    for (size_t i = 0; i < G_N_ELEMENTS(LIBRARY_FAMILIES); i++) {
        g_autoptr(GPtrArray) argv = NULL;

        if (!mfr_capture_job_check_captured(job, LIBRARY_FAMILIES[i].soname))
            continue;
        argv = mfr_capture_job_make_argv(job, job->libdir);
        for (size_t j = 0; LIBRARY_FAMILIES[i].relatives[j] != NULL; j++)
            g_ptr_array_add(argv, g_strdup(LIBRARY_FAMILIES[i].relatives[j]));
        if (!mfr_capture_run_tool(argv, NULL, error))
            return glnx_prefix_error(error,
                                     "Unable to capture the relatives of %s",
                                     LIBRARY_FAMILIES[i].soname);
    }

    return TRUE;
}

/**
 * mfr_find_loader_manifests:
 *
 * Find the host's loader manifests of one kind, sorted by file name. If several
 * directories have a file of the same name, only the one from the directory
 * with the highest priority is used, as the loaders do.
 *
 * Returns: (transfer full): paths of the manifests
 */
static GPtrArray *
mfr_find_loader_manifests(const MfrLoaderManifestKind *kind)
{
    g_autoptr(GHashTable) found = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_autoptr(GList) names = NULL;
    GPtrArray *sources = g_ptr_array_new_with_free_func(g_free);

    for (size_t i = 0; kind->dirs[i] != NULL; i++) {
        g_autoptr(GDir) entries = g_dir_open(kind->dirs[i], 0, NULL);
        const char *name;

        while (entries != NULL && (name = g_dir_read_name(entries)) != NULL) {
            if (g_str_has_suffix(name, kind->suffix) && !g_hash_table_contains(found, name))
                g_hash_table_insert(found, g_strdup(name), g_build_filename(kind->dirs[i], name, NULL));
        }
    }

    names = g_list_sort(g_hash_table_get_keys(found), (GCompareFunc) g_strcmp0);
    for (GList *l = names; l != NULL; l = l->next)
        g_ptr_array_add(sources, g_strdup(g_hash_table_lookup(found, l->data)));

    return sources;
}

/**
 * mfr_loader_manifest_get_library_path:
 *
 * Returns: the library_path of a loader-manifest object, or %NULL if it has none
 */
static const char *
mfr_loader_manifest_get_library_path(sd_json_variant *slot)
{
    sd_json_variant *value = sd_json_variant_by_key(slot, "library_path");

    if (!sd_json_variant_is_string(value) || sd_json_variant_string(value)[0] == '\0')
        return NULL;
    return sd_json_variant_string(value);
}

/**
 * MfrLibraryPathFunc:
 * @library_path: the library_path of an object of a loader manifest
 * @user_data: what was passed to mfr_loader_manifest_map_library_paths()
 * @error: a #GError
 *
 * Returns: (transfer full): the library_path to put in its place, or %NULL on error
 */
typedef char *(*MfrLibraryPathFunc)(const char *library_path, gpointer user_data, GError **error);

/**
 * mfr_loader_manifest_object_map_library_path:
 *
 * Pass the library_path of one object of a loader manifest through @func, if
 * the object has one, and put what @func returns in its place.
 *
 * Returns: %TRUE, or %FALSE if @func or the rewrite failed
 */
static gboolean
mfr_loader_manifest_object_map_library_path(sd_json_variant **object,
                                            MfrLibraryPathFunc func,
                                            gpointer user_data,
                                            GError **error)
{
    const char *library_path = sd_json_variant_is_object(*object)
                                   ? mfr_loader_manifest_get_library_path(*object)
                                   : NULL;
    g_autofree char *replacement = NULL;
    int r;

    if (library_path == NULL)
        return TRUE;
    replacement = func(library_path, user_data, error);
    if (replacement == NULL)
        return FALSE;
    r = sd_json_variant_set_field_string(object, "library_path", replacement);
    if (r < 0)
        return glnx_throw(error, "Unable to rewrite manifest: %s", g_strerror(-r));
    return TRUE;
}

/**
 * mfr_loader_manifest_map_library_paths:
 * @manifest: a JSON loader manifest
 *
 * Pass every library_path of a JSON loader manifest through @func: the one of
 * the ICD, of the layer, or of each of the layers. Meta-layers have none.
 *
 * Returns: (transfer full): a copy of @manifest with what @func returned in
 *   place of every library_path, or %NULL on error
 */
static sd_json_variant *
mfr_loader_manifest_map_library_paths(sd_json_variant *manifest,
                                      MfrLibraryPathFunc func,
                                      gpointer user_data,
                                      GError **error)
{
    g_autoptr(sd_json_variant) result = sd_json_variant_ref(manifest);
    int r;

    for (const char *const *key = (const char *const[]){"ICD", "layer", "layers", NULL}; *key != NULL;
         key++) {
        g_autoptr(sd_json_variant) value = sd_json_variant_ref(sd_json_variant_by_key(result, *key));

        if (value == NULL)
            continue;
        if (sd_json_variant_is_array(value)) {
            g_autoptr(sd_json_variant) elements = NULL;

            for (size_t i = 0; i < sd_json_variant_elements(value); i++) {
                g_autoptr(sd_json_variant) element = sd_json_variant_ref(sd_json_variant_by_index(value, i));

                if (!mfr_loader_manifest_object_map_library_path(&element, func, user_data, error))
                    return NULL;
                r = sd_json_variant_append_array(&elements, element);
                if (r < 0)
                    return glnx_null_throw(error, "Unable to rewrite manifest: %s", g_strerror(-r));
            }
            /* an empty array stays as it is */
            if (elements == NULL)
                continue;
            sd_json_variant_unref(value);
            value = g_steal_pointer(&elements);
        } else if (!mfr_loader_manifest_object_map_library_path(&value, func, user_data, error)) {
            return NULL;
        }

        r = sd_json_variant_set_field(&result, *key, value);
        if (r < 0)
            return glnx_null_throw(error, "Unable to rewrite manifest: %s", g_strerror(-r));
    }

    return g_steal_pointer(&result);
}

/**
 * mfr_loader_manifest_libpath_get_host_path:
 *
 * Get the host path of a library a manifest names, if it names one by path:
 * absolute, or relative to the manifest at @source.
 *
 * Returns: %TRUE with the path in @ret_path, %FALSE if it is a SONAME
 */
static gboolean
mfr_loader_manifest_libpath_get_host_path(const char *library_path, const char *source, char **ret_path)
{
    if (library_path[0] == '/') {
        *ret_path = g_strdup(library_path);
        return TRUE;
    }
    if (strchr(library_path, '/') != NULL) {
        g_autofree char *dir = g_path_get_dirname(source);
        g_autofree char *joined = g_build_filename(dir, library_path, NULL);

        *ret_path = g_canonicalize_filename(joined, "/");
        return TRUE;
    }
    return FALSE;
}

/**
 * mfr_loader_manifest_plan_add_library_cb:
 *
 * Sort a library that a manifest names into the SONAMEs or the host paths of
 * a plan, see mfr_loader_manifest_map_library_paths().
 *
 * Returns: (transfer full): @library_path, unchanged
 */
static char *
mfr_loader_manifest_plan_add_library_cb(const char *library_path, gpointer user_data, GError **error)
{
    MfrLoaderManifestPlan *plan = user_data;
    char *path = NULL;

    if (mfr_loader_manifest_libpath_get_host_path(library_path, plan->source, &path))
        g_ptr_array_add(plan->absolute, path);
    else
        g_ptr_array_add(plan->sonames, g_strdup(library_path));
    return g_strdup(library_path);
}

/**
 * mfr_loader_manifest_plan_new:
 *
 * Read a manifest of the host and sort the libraries it names into SONAMEs
 * and host paths. We keep a JSON manifest, so we only parse it once.
 *
 * Returns: (transfer full) (nullable): the plan, or %NULL if the manifest is unreadable
 */
static MfrLoaderManifestPlan *
mfr_loader_manifest_plan_new(const MfrLoaderManifestKind *kind, const char *source)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(sd_json_variant) planned = NULL;
    MfrLoaderManifestPlan *plan;

    plan = g_new0(MfrLoaderManifestPlan, 1);
    plan->kind = kind;
    plan->source = g_strdup(source);
    plan->sonames = g_ptr_array_new_with_free_func(g_free);
    plan->absolute = g_ptr_array_new_with_free_func(g_free);

    if (!kind->is_json) {
        /* an OpenCL .icd file is one line: a SONAME or an absolute path */
        g_autofree char *entry = NULL;

        if (!g_file_get_contents(source, &entry, NULL, &error)) {
            g_warning("Unable to read %s: %s", source, error->message);
            mfr_loader_manifest_plan_free(plan);
            return NULL;
        }
        g_strstrip(entry);
        if (entry[0] == '/')
            g_ptr_array_add(plan->absolute, g_steal_pointer(&entry));
        else if (entry[0] != '\0')
            g_ptr_array_add(plan->sonames, g_steal_pointer(&entry));
        return plan;
    }

    /* we only need what the walk sorts into the plan, not the copy of the manifest it returns */
    plan->manifest = mf_json_parse_file(source, &error);
    if (plan->manifest != NULL)
        planned = mfr_loader_manifest_map_library_paths(plan->manifest,
                                                        mfr_loader_manifest_plan_add_library_cb,
                                                        plan,
                                                        &error);
    if (planned == NULL) {
        g_warning("Unable to parse %s: %s", source, error->message);
        mfr_loader_manifest_plan_free(plan);
        return NULL;
    }

    return plan;
}

/**
 * mfr_fetch_loader_manifest_groups:
 *
 * Returns: (transfer full): plans for every loader manifest of the host,
 *   grouped by kind, for the kinds the host has readable manifests of
 */
static GPtrArray *
mfr_fetch_loader_manifest_groups(void)
{
    GPtrArray *groups = g_ptr_array_new_with_free_func((GDestroyNotify) mfr_loader_manifest_group_free);

    for (size_t i = 0; i < G_N_ELEMENTS(MFR_LOADER_MANIFEST_KINDS); i++) {
        const MfrLoaderManifestKind *kind = &MFR_LOADER_MANIFEST_KINDS[i];
        g_autoptr(GPtrArray) sources = mfr_find_loader_manifests(kind);
        g_autoptr(GPtrArray) plans = NULL;
        MfrLoaderManifestGroup *group;

        if (sources->len == 0)
            continue;
        g_info("%s: %u manifest(s) on the host", kind->label, sources->len);

        plans = g_ptr_array_new_with_free_func((GDestroyNotify) mfr_loader_manifest_plan_free);
        for (guint j = 0; j < sources->len; j++) {
            auto plan = mfr_loader_manifest_plan_new(kind, g_ptr_array_index(sources, j));

            if (plan != NULL)
                g_ptr_array_add(plans, plan);
        }
        if (plans->len == 0)
            continue;

        group = g_new0(MfrLoaderManifestGroup, 1);
        group->kind = kind;
        group->plans = g_steal_pointer(&plans);
        g_ptr_array_add(groups, group);
    }

    return groups;
}

/**
 * mfr_loader_manifest_groups_register_patterns:
 *
 * Register the patterns that the manifests need for the main run. We capture
 * the libraries that a manifest names by SONAME into the libdir. Of the
 * libraries named by path, we only capture their dependencies here, as
 * drivers of different vendors may have the same file name.
 * mfr_capture_job_place_drivers() captures those libraries themselves, into
 * subdirectories that are numbered where names clash.
 */
static void
mfr_loader_manifest_groups_register_patterns(GPtrArray *groups, GPtrArray *argv)
{
    for (guint i = 0; i < groups->len; i++) {
        const MfrLoaderManifestGroup *group = g_ptr_array_index(groups, i);

        for (guint j = 0; j < group->plans->len; j++) {
            const MfrLoaderManifestPlan *plan = g_ptr_array_index(group->plans, j);

            for (guint k = 0; k < plan->sonames->len; k++)
                g_ptr_array_add(argv,
                                g_strdup_printf("if-exists:even-if-older:if-same-abi:soname:%s",
                                                (char *) g_ptr_array_index(plan->sonames, k)));
            for (guint k = 0; k < plan->absolute->len; k++)
                g_ptr_array_add(argv,
                                g_strdup_printf("only-dependencies:if-exists:if-same-abi:path:%s",
                                                (char *) g_ptr_array_index(plan->absolute, k)));
        }
    }
}

/**
 * mfr_capture_job_resolve_library_path:
 *
 * Returns: (transfer full) (nullable): the path in the tree a manifest's
 *   library_path has to be rewritten to, or %NULL if the library was not captured
 */
static char *
mfr_capture_job_resolve_library_path(MfrCaptureJob *job,
                                     const MfrLoaderManifestPlan *plan,
                                     const char *library_path)
{
    g_autofree char *path = NULL;

    if (mfr_loader_manifest_libpath_get_host_path(library_path, plan->source, &path))
        return g_strdup(g_hash_table_lookup(job->placed, path));
    /* The main run captured every library a manifest names by SONAME, but only
     * if the host has it for this architecture. Without a link to it in the
     * libdir, the manifest is for another architecture, e.g. a 32-bit driver,
     * and is left out. */
    if (!mfr_capture_job_check_captured(job, library_path))
        return NULL;
    return g_build_filename(job->tree_libdir, library_path, NULL);
}

/* what mfr_capture_job_rewrite_library_path() rewrites a library_path for */
typedef struct {
    MfrCaptureJob *job;
    const MfrLoaderManifestPlan *plan;
} MfrLibraryPathRewrite;

/**
 * mfr_capture_job_rewrite_library_path_cb:
 *
 * Returns: (transfer full): the path in the tree a library_path has to be
 *   rewritten to, or %NULL if the library was not captured
 */
static char *
mfr_capture_job_rewrite_library_path_cb(const char *library_path, gpointer user_data, GError **error)
{
    const MfrLibraryPathRewrite *rewrite = user_data;
    char *resolved = mfr_capture_job_resolve_library_path(rewrite->job, rewrite->plan, library_path);

    if (resolved == NULL)
        glnx_throw(error, "%s was not captured", library_path);
    return resolved;
}

/**
 * mfr_capture_job_rewrite_json_loader_manifest:
 *
 * Write a copy of a JSON manifest to @target, with every library_path
 * pointing into the tree.
 */
static gboolean
mfr_capture_job_rewrite_json_loader_manifest(MfrCaptureJob *job,
                                             const MfrLoaderManifestPlan *plan,
                                             const char *target,
                                             GError **error)
{
    MfrLibraryPathRewrite rewrite = {.job = job, .plan = plan};
    g_autoptr(sd_json_variant) manifest = NULL;

    manifest = mfr_loader_manifest_map_library_paths(plan->manifest,
                                                     mfr_capture_job_rewrite_library_path_cb,
                                                     &rewrite,
                                                     error);
    if (manifest == NULL)
        return FALSE;
    return mf_json_write_file(target, manifest, error);
}

/**
 * mfr_capture_job_rewrite_icd_file:
 *
 * Write a copy of an OpenCL .icd file to @target, pointing into the tree.
 */
static gboolean
mfr_capture_job_rewrite_icd_file(MfrCaptureJob *job,
                                 const MfrLoaderManifestPlan *plan,
                                 const char *target,
                                 GError **error)
{
    g_autofree char *entry = NULL;
    g_autofree char *line = NULL;

    if (plan->absolute->len > 0) {
        const char *library_path = g_ptr_array_index(plan->absolute, 0);

        entry = mfr_capture_job_resolve_library_path(job, plan, library_path);
        if (entry == NULL)
            return glnx_throw(error, "%s was not captured", library_path);
    } else if (plan->sonames->len > 0) {
        /* the file is shared between architectures, and ld.so.cache resolves a SONAME
         * for the architecture that loads it */
        entry = g_strdup(g_ptr_array_index(plan->sonames, 0));
    } else {
        return glnx_throw(error, "it names no library");
    }

    line = g_strconcat(entry, "\n", NULL);
    return g_file_set_contents(target, line, -1, error);
}

/**
 * mfr_capture_job_place_drivers:
 * @libsubdir: the subdirectory of the libdir to place into
 *
 * Capture the libraries that the manifests of every group placing into
 * @libsubdir name by path, and record where they are in the tree.
 * Drivers of different vendors may have the same file name, so if any two
 * do, every driver gets a numbered subdirectory of its own. A driver that
 * several manifests name is placed once, and does not clash with itself.
 */
static gboolean
mfr_capture_job_place_drivers(MfrCaptureJob *job, GPtrArray *groups, const char *libsubdir, GError **error)
{
    g_autoptr(GPtrArray) paths = g_ptr_array_new();
    g_autoptr(GHashTable) basenames = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    gboolean numbered = FALSE;

    for (guint i = 0; i < groups->len; i++) {
        const MfrLoaderManifestGroup *group = g_ptr_array_index(groups, i);

        if (!g_str_equal(group->kind->libsubdir, libsubdir))
            continue;
        for (guint j = 0; j < group->plans->len; j++) {
            const MfrLoaderManifestPlan *plan = g_ptr_array_index(group->plans, j);

            for (guint k = 0; k < plan->absolute->len; k++) {
                const char *path = g_ptr_array_index(plan->absolute, k);

                if (g_ptr_array_find_with_equal_func(paths, path, g_str_equal, NULL))
                    continue;
                g_ptr_array_add(paths, (gpointer) path);
                if (!g_hash_table_add(basenames, g_path_get_basename(path)))
                    numbered = TRUE;
            }
        }
    }

    for (guint i = 0; i < paths->len; i++) {
        const char *path = g_ptr_array_index(paths, i);
        g_autofree char *basename = g_path_get_basename(path);
        g_autofree char *subdir = numbered ? g_strdup_printf("%s/%u", libsubdir, i) : g_strdup(libsubdir);
        g_autofree char *driver_dir = g_build_filename(job->libdir, subdir, NULL);
        g_autofree char *link = g_build_filename(driver_dir, basename, NULL);
        g_autoptr(GPtrArray) argv = NULL;

        if (g_hash_table_contains(job->placed, path))
            continue;
        if (!glnx_shutil_mkdir_p_at(AT_FDCWD, driver_dir, 0755, NULL, error))
            return FALSE;
        argv = mfr_capture_job_make_argv(job, driver_dir);
        g_ptr_array_add(argv,
                        g_strdup_printf("no-dependencies:even-if-older:if-exists:if-same-abi:path:%s", path));
        if (!mfr_capture_run_tool(argv, NULL, error))
            return FALSE;
        /* if-exists and if-same-abi may have left nothing behind */
        if (g_file_test(link, G_FILE_TEST_IS_SYMLINK))
            g_hash_table_insert(job->placed,
                                g_strdup(path),
                                g_build_filename(job->tree_libdir, subdir, basename, NULL));
    }

    return TRUE;
}

/**
 * mfr_capture_job_install_loader_manifests:
 *
 * Capture the libraries the manifests name by path, write the rewritten
 * manifests to the overrides and point the loaders at them. Every
 * library_path is rewritten to an absolute path in the tree, so a loader
 * never picks a library of another architecture or the image's own.
 */
static gboolean
mfr_capture_job_install_loader_manifests(MfrCaptureJob *job, GPtrArray *groups, GError **error)
{
    g_autoptr(GHashTable) libsubdirs = g_hash_table_new(g_str_hash, g_str_equal);

    /* kinds may share a subdirectory, whose drivers are placed together */
    for (guint i = 0; i < groups->len; i++) {
        const MfrLoaderManifestGroup *group = g_ptr_array_index(groups, i);

        if (g_hash_table_add(libsubdirs, (char *) group->kind->libsubdir) &&
            !mfr_capture_job_place_drivers(job, groups, group->kind->libsubdir, error))
            return FALSE;
    }

    for (guint i = 0; i < groups->len; i++) {
        const MfrLoaderManifestGroup *group = g_ptr_array_index(groups, i);
        const MfrLoaderManifestKind *kind = group->kind;
        g_autofree char *dest_rel = NULL;
        g_autofree char *dest_dir = NULL;
        unsigned int written = 0;

        dest_rel = g_build_filename(GRAPHICS_OVERRIDES,
                                    kind->per_arch ? job->arch_tuple : "common",
                                    kind->dest,
                                    NULL);
        dest_dir = g_build_filename(job->overrides_dir, dest_rel, NULL);
        if (!glnx_shutil_mkdir_p_at(AT_FDCWD, dest_dir, 0755, NULL, error))
            return FALSE;

        for (guint j = 0; j < group->plans->len; j++) {
            const MfrLoaderManifestPlan *plan = g_ptr_array_index(group->plans, j);
            g_autofree char *basename = g_path_get_basename(plan->source);
            g_autofree char *name = g_strdup_printf("%02u-%s", j, basename);
            g_autofree char *target = g_build_filename(dest_dir, name, NULL);
            g_autofree char *tree_path = g_build_filename(dest_rel, name, NULL);
            g_autoptr(GError) rewrite_error = NULL;
            gboolean ok;

            ok = kind->is_json
                     ? mfr_capture_job_rewrite_json_loader_manifest(job, plan, target, &rewrite_error)
                     : mfr_capture_job_rewrite_icd_file(job, plan, target, &rewrite_error);
            if (!ok) {
                g_info("Leaving %s out: %s", plan->source, rewrite_error->message);
                continue;
            }
            written++;
            for (size_t k = 0; kind->env_files[k] != NULL; k++)
                mfr_env_table_add(job->env, kind->env_files[k], tree_path);
        }
        if (written > 0) {
            for (size_t k = 0; kind->env_dirs[k] != NULL; k++)
                mfr_env_table_add(job->env, kind->env_dirs[k], dest_rel);
        }
        g_info("%s: %u manifest(s) installed", kind->label, written);
    }

    return TRUE;
}

/**
 * mfr_capture_job_link_to_host:
 * @relative_link: where the link goes, relative to the tree
 * @host_path: the absolute path on the host it points to
 *
 * Link a path of the tree to one of the host.
 */
static gboolean
mfr_capture_job_link_to_host(MfrCaptureJob *job,
                             const char *relative_link,
                             const char *host_path,
                             GError **error)
{
    g_autofree char *link = g_build_filename(job->overrides_dir, relative_link, NULL);
    g_autofree char *target = g_build_filename(MFR_HOST_DIR, host_path, NULL);
    g_autofree char *parent = g_path_get_dirname(link);

    if (!glnx_shutil_mkdir_p_at(AT_FDCWD, parent, 0755, NULL, error))
        return FALSE;
    if (symlink(target, link) != 0)
        return glnx_throw_errno_prefix(error, "Unable to link %s", link);
    return TRUE;
}

/**
 * mfr_capture_job_link_host_path:
 *
 * Link a path of the tree to the host's, if the host has it and it passes @test.
 */
static gboolean
mfr_capture_job_link_host_path(MfrCaptureJob *job, const char *relative_path, GFileTest test, GError **error)
{
    g_autofree char *host_path = g_build_filename("/", relative_path, NULL);

    if (!g_file_test(host_path, test))
        return TRUE;
    return mfr_capture_job_link_to_host(job, relative_path, host_path, error);
}

/**
 * mfr_capture_job_link_host_dir:
 *
 * Link a directory of the tree to the host's, if the host has it.
 */
static gboolean
mfr_capture_job_link_host_dir(MfrCaptureJob *job, const char *relative_path, GError **error)
{
    return mfr_capture_job_link_host_path(job, relative_path, G_FILE_TEST_IS_DIR, error);
}

/**
 * mfr_capture_job_link_host_executable:
 *
 * Link an executable of the tree to the host's, if the host has it.
 */
static gboolean
mfr_capture_job_link_host_executable(MfrCaptureJob *job, const char *relative_path, GError **error)
{
    return mfr_capture_job_link_host_path(job, relative_path, G_FILE_TEST_IS_EXECUTABLE, error);
}

/**
 * mfr_capture_job_ask_tool:
 *
 * Run the capture tool with an option that makes it print a path.
 *
 * Returns: (transfer full): the path, or %NULL on error
 */
static char *
mfr_capture_job_ask_tool(MfrCaptureJob *job, const char *option, GError **error)
{
    g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
    char *output = NULL;

    g_ptr_array_add(argv, g_strdup(job->tool));
    g_ptr_array_add(argv, g_strdup(option));
    if (!mfr_capture_run_tool(argv, &output, error))
        return NULL;
    return g_strchomp(output);
}

/**
 * mfr_capture_job_resolve_core_path:
 * @path: an absolute path, as the tree sees it
 *
 * Resolve @path within the core. An override has to replace what the path
 * ends at, not a link of the core on the way, which would hide everything
 * the link leads to.
 *
 * Returns: (transfer full): the path, relative to the core
 */
static char *
mfr_capture_job_resolve_core_path(MfrCaptureJob *job, const char *path)
{
    /* links are followed as if the core was the root, so an absolute one stays within it */
    struct open_how how = {.flags = O_PATH | O_CLOEXEC, .resolve = RESOLVE_IN_ROOT};
    glnx_autofd int core_fd = open(job->core_mount, O_PATH | O_DIRECTORY | O_CLOEXEC);
    glnx_autofd int fd = core_fd >= 0 ? (int) syscall(SYS_openat2, core_fd, path, &how, sizeof(how)) : -1;
    g_autofree char *fd_path = g_strdup_printf("/proc/self/fd/%d", fd);
    g_autofree char *resolved = fd >= 0 ? g_file_read_link(fd_path, NULL) : NULL;

    if (resolved != NULL && g_str_has_prefix(resolved, job->core_mount) &&
        resolved[strlen(job->core_mount)] == '/')
        return g_strdup(resolved + strlen(job->core_mount) + 1);
    /* missing in the core: use the path as-is */
    return g_strdup(g_path_skip_root(path));
}

/**
 * mfr_capture_job_get_host_libc_dir:
 *
 * Returns: (transfer full) (nullable): the canonical directory of the
 *   libc.so.6 the main run took from the host
 */
static char *
mfr_capture_job_get_host_libc_dir(MfrCaptureJob *job)
{
    g_autofree char *link = g_build_filename(job->libdir, "libc.so.6", NULL);
    g_autofree char *target = g_file_read_link(link, NULL);
    g_autofree char *dir = NULL;

    if (target == NULL || !g_str_has_prefix(target, MFR_HOST_DIR "/"))
        return NULL;
    dir = g_path_get_dirname(target + strlen(MFR_HOST_DIR));
    return realpath(dir, NULL);
}

/**
 * mfr_capture_job_link_host_glibc_runtime:
 *
 * If glibc came from the host, link what has to match its version to the
 * host's too: the dynamic linker, the locales, the gconv modules and the
 * glibc programs.
 */
static gboolean
mfr_capture_job_link_host_glibc_runtime(MfrCaptureJob *job, GError **error)
{
    g_autofree char *ld_so = NULL;
    g_autofree char *resolved = NULL;
    g_autofree char *ld_so_in_core = NULL;
    g_autofree char *link = NULL;
    g_autofree char *libc_dir = NULL;
    g_autofree char *libc_dir_in_core = NULL;
    g_autofree char *gconv = NULL;
    g_autofree char *gconv_in_core = NULL;

    if (!mfr_capture_job_check_captured(job, "libc.so.6")) {
        g_info("glibc comes from the image, keeping its dynamic linker");
        return TRUE;
    }

    /* the tool knows the path of the dynamic linker for its architecture, which is the
     * same on the host and in every image we install */
    ld_so = mfr_capture_job_ask_tool(job, "--print-ld.so", error);
    if (ld_so == NULL)
        return FALSE;
    resolved = mfr_capture_job_ask_tool(job, "--resolve-ld.so=/", error);
    if (resolved == NULL)
        return FALSE;

    ld_so_in_core = mfr_capture_job_resolve_core_path(job, ld_so);
    link = g_build_filename(job->overrides_dir, ld_so_in_core, NULL);
    /* the main run may have linked it as part of the glibc family */
    (void) unlink(link);
    if (!mfr_capture_job_link_to_host(job, ld_so_in_core, resolved, error))
        return FALSE;
    g_info("Dynamic linker comes from the host: %s replaces %s", resolved, ld_so_in_core);

    /* glibc looks for its conversion modules next to itself, in whatever the host calls its libdir */
    libc_dir = mfr_capture_job_get_host_libc_dir(job);
    if (libc_dir == NULL)
        return glnx_throw(error, "Unable to find the directory of the host's libc.so.6");
    gconv = g_build_filename(libc_dir, "gconv", NULL);
    libc_dir_in_core = mfr_capture_job_resolve_core_path(job, libc_dir);
    gconv_in_core = g_build_filename(libc_dir_in_core, "gconv", NULL);

    /* a link to a directory of the host replaces the image's directory in the overlay */
    if (!mfr_capture_job_link_host_dir(job, "usr/lib/locale", error) ||
        !mfr_capture_job_link_host_dir(job, "usr/share/i18n", error))
        return FALSE;
    if (g_file_test(gconv, G_FILE_TEST_IS_DIR) &&
        !mfr_capture_job_link_to_host(job, gconv_in_core, gconv, error))
        return FALSE;
    for (size_t i = 0; i < G_N_ELEMENTS(GLIBC_EXECUTABLES); i++) {
        if (!mfr_capture_job_link_host_executable(job, GLIBC_EXECUTABLES[i], error))
            return FALSE;
    }
    g_info("Locales, conversion modules and the glibc programs come from the host");

    return TRUE;
}

/**
 * mfr_capture_job_link_host_data_dirs:
 *
 * Link the data directories of LIBRARY_DATA_DIRS libraries that came from
 * the host to the host's within our runtime environment.
 */
static gboolean
mfr_capture_job_link_host_data_dirs(MfrCaptureJob *job, GError **error)
{
    g_autoptr(GHashTable) linked = g_hash_table_new(g_str_hash, g_str_equal);

    for (size_t i = 0; i < G_N_ELEMENTS(LIBRARY_DATA_DIRS); i++) {
        g_autofree char *dir = g_build_filename("usr/share", LIBRARY_DATA_DIRS[i].dir, NULL);

        if (g_hash_table_contains(linked, LIBRARY_DATA_DIRS[i].dir) ||
            !mfr_capture_job_check_captured(job, LIBRARY_DATA_DIRS[i].soname))
            continue;
        if (!mfr_capture_job_link_host_dir(job, dir, error))
            return FALSE;
        g_hash_table_add(linked, (char *) LIBRARY_DATA_DIRS[i].dir);
    }

    return TRUE;
}

/**
 * mfr_capture_job_add_driver_path_vars:
 *
 * Point the DRI, GBM, VA-API and VDPAU loaders at the captured drivers.
 */
static void
mfr_capture_job_add_driver_path_vars(MfrCaptureJob *job)
{
    for (size_t i = 0; i < G_N_ELEMENTS(DRIVER_KINDS); i++) {
        g_autofree char *dir = g_build_filename(job->drivers_dir, DRIVER_KINDS[i].subdir, NULL);
        g_autofree char *tree_dir = g_build_filename(job->tree_drivers_dir, DRIVER_KINDS[i].subdir, NULL);

        if (!g_file_test(dir, G_FILE_TEST_IS_DIR))
            continue;
        for (size_t j = 0; DRIVER_KINDS[i].variables[j] != NULL; j++)
            mfr_env_table_add(job->env, DRIVER_KINDS[i].variables[j], tree_dir);
    }
}

/**
 * mfr_stat_mtime_ns:
 *
 * Returns: the modification time of @st, in nanoseconds
 */
static guint64
mfr_stat_mtime_ns(const struct stat *st)
{
    return (guint64) st->st_mtim.tv_sec * G_USEC_PER_SEC * 1000 + (guint64) st->st_mtim.tv_nsec;
}

/**
 * mfr_stamp_add_dir:
 *
 * Add a host directory and its modification time (0 if it is missing) to
 * the stamp entries @dirs.
 */
static int
mfr_stamp_add_dir(sd_json_variant **dirs, const char *path)
{
    struct stat st;
    guint64 mtime = stat(path, &st) == 0 ? mfr_stat_mtime_ns(&st) : 0;

    return sd_json_variant_append_arraybo(dirs,
                                          SD_JSON_BUILD_PAIR_STRING("path", path),
                                          SD_JSON_BUILD_PAIR_UNSIGNED("mtime", mtime));
}

/**
 * MfrStampTargets:
 *
 * The host files the links of a capture point at, each recorded once.
 */
typedef struct {
    sd_json_variant *entries; /* path, size and modification time of each */
    GHashTable *paths;        /* their paths, as a set */
} MfrStampTargets;

/**
 * mfr_stamp_targets_clear:
 */
static void
mfr_stamp_targets_clear(MfrStampTargets *targets)
{
    g_clear_pointer(&targets->entries, sd_json_variant_unref);
    g_clear_pointer(&targets->paths, g_hash_table_unref);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(MfrStampTargets, mfr_stamp_targets_clear)

/**
 * mfr_stamp_targets_add:
 *
 * Add a host file to @targets, with its size and modification time,
 * unless it is in there already.
 */
static gboolean
mfr_stamp_targets_add(MfrStampTargets *targets, const char *host_path, GError **error)
{
    struct stat st;
    int r;

    if (!g_hash_table_add(targets->paths, g_strdup(host_path)))
        return TRUE;
    if (stat(host_path, &st) != 0)
        return glnx_throw_errno_prefix(error, "Captured %s is missing on the host", host_path);
    r = sd_json_variant_append_arraybo(&targets->entries,
                                       SD_JSON_BUILD_PAIR_STRING("path", host_path),
                                       SD_JSON_BUILD_PAIR_UNSIGNED("size", (guint64) st.st_size),
                                       SD_JSON_BUILD_PAIR_UNSIGNED("mtime", mfr_stat_mtime_ns(&st)));
    if (r < 0)
        return glnx_throw(error, "Unable to record %s: %s", host_path, g_strerror(-r));

    return TRUE;
}

/**
 * mfr_stamp_add_link_targets:
 *
 * Add every host file a link below @dir points at to @targets, with its
 * size and modification time.
 */
static gboolean
mfr_stamp_add_link_targets(const char *dir, MfrStampTargets *targets, GError **error)
{
    g_auto(GLnxDirFdIterator) iter = {0};
    struct dirent *entry;

    if (!glnx_dirfd_iterator_init_at(AT_FDCWD, dir, FALSE, &iter, error))
        return FALSE;
    while (TRUE) {
        g_autofree char *path = NULL;

        if (!glnx_dirfd_iterator_next_dent_ensure_dtype(&iter, &entry, NULL, error))
            return FALSE;
        if (entry == NULL)
            break;

        path = g_build_filename(dir, entry->d_name, NULL);
        if (entry->d_type == DT_DIR) {
            if (!mfr_stamp_add_link_targets(path, targets, error))
                return FALSE;
        } else if (entry->d_type == DT_LNK) {
            g_autofree char *target = g_file_read_link(path, NULL);

            if (target == NULL || !g_str_has_prefix(target, MFR_HOST_DIR))
                continue;
            if (!mfr_stamp_targets_add(targets, target + strlen(MFR_HOST_DIR), error))
                return FALSE;
        }
    }

    return TRUE;
}

/**
 * mfr_capture_job_write_stamp:
 *
 * Write the stamp of the capture, which records what it depends on on the host.
 */
static gboolean
mfr_capture_job_write_stamp(MfrCaptureJob *job, const char *dir, GError **error)
{
    g_autoptr(sd_json_variant) stamp = NULL;
    g_autoptr(sd_json_variant) dirs = NULL;
    g_auto(MfrStampTargets) targets = {
        .paths = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL),
    };
    g_autofree char *host_libdir = g_build_filename("/usr/lib", job->arch_tuple, NULL);
    g_autofree char *id = g_uuid_string_random();
    g_autofree char *path = g_build_filename(dir, STAMP_NAME, NULL);
    int r;

    if (mfr_stamp_add_dir(&dirs, host_libdir) < 0)
        return glnx_throw(error, "Unable to record directories");
    for (guint i = 0; i < job->host_dirs->len; i++) {
        const char *host_dir = g_ptr_array_index(job->host_dirs, i);

        if (!g_str_equal(host_dir, host_libdir) && mfr_stamp_add_dir(&dirs, host_dir) < 0)
            return glnx_throw(error, "Unable to record directories");
    }
    /* the manifests are copied, not linked: adding, removing or replacing one changes its directory */
    for (size_t i = 0; i < G_N_ELEMENTS(MFR_LOADER_MANIFEST_KINDS); i++) {
        for (size_t j = 0; MFR_LOADER_MANIFEST_KINDS[i].dirs[j] != NULL; j++) {
            if (mfr_stamp_add_dir(&dirs, MFR_LOADER_MANIFEST_KINDS[i].dirs[j]) < 0)
                return glnx_throw(error, "Unable to record directories");
        }
    }
    if (!mfr_stamp_add_link_targets(job->overrides_dir, &targets, error))
        return FALSE;
    if (targets.entries == NULL)
        r = sd_json_variant_new_array(&targets.entries, NULL, 0);
    else
        r = 0;
    if (r >= 0)
        r = sd_json_buildo(&stamp,
                           SD_JSON_BUILD_PAIR_STRING("id", id),
                           SD_JSON_BUILD_PAIR_STRING("tuple", job->arch_tuple),
                           SD_JSON_BUILD_PAIR_VARIANT("dirs", dirs),
                           SD_JSON_BUILD_PAIR_VARIANT("targets", targets.entries));
    if (r < 0)
        return glnx_throw(error, "Unable to build stamp: %s", g_strerror(-r));

    return mf_json_write_file(path, stamp, error);
}

/**
 * mfr_capture_job_write_environment:
 *
 * Write the environment the capture needs, one entry per managed variable.
 */
static gboolean
mfr_capture_job_write_environment(MfrCaptureJob *job, const char *dir, GError **error)
{
    g_auto(GStrv) variables = mfr_capture_get_managed_variables();
    g_autoptr(sd_json_variant) environment = NULL;
    g_autofree char *path = g_build_filename(dir, ENVIRONMENT_NAME, NULL);
    int r;

    for (size_t i = 0; variables[i] != NULL; i++) {
        GPtrArray *values = g_hash_table_lookup(job->env, variables[i]);
        g_autofree char *joined = NULL;

        if (values != NULL) {
            g_ptr_array_add(values, NULL);
            joined = g_strjoinv(":", (char **) values->pdata);
            g_ptr_array_remove_index(values, values->len - 1);
        }
        r = sd_json_variant_set_field_string(&environment, variables[i], joined != NULL ? joined : "");
        if (r < 0)
            return glnx_throw(error, "Unable to build environment: %s", g_strerror(-r));
    }

    return mf_json_write_file(path, environment, error);
}

/**
 * mfr_capture_build:
 *
 * Capture the host's graphics stack for @arch_tuple into @dir, against the core
 * mounted at @core_mount.
 */
static gboolean
mfr_capture_build(const char *dir, const char *core_mount, const char *arch_tuple, GError **error)
{
    MfrCaptureJob job = {
        .tool = MF_CAPSULE_BIN_DIR "/capsule-capture-libs",
        .core_mount = core_mount,
        .arch_tuple = arch_tuple,
    };
    g_autoptr(GPtrArray) loader_manifests = NULL;
    g_autoptr(GPtrArray) argv = NULL;
    gboolean ret = FALSE;

    job.overrides_dir = g_build_filename(dir, OVERRIDES_NAME, NULL);
    job.libdir = g_build_filename(job.overrides_dir, "usr/lib", arch_tuple, NULL);
    job.tree_libdir = g_build_filename("/usr/lib", arch_tuple, NULL);
    job.tree_drivers_dir = g_build_filename(GRAPHICS_OVERRIDES, arch_tuple, NULL);
    job.drivers_dir = g_build_filename(job.overrides_dir, job.tree_drivers_dir, NULL);
    job.env = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_ptr_array_unref);
    job.placed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    job.host_dirs = g_ptr_array_new_with_free_func(g_free);
    for (size_t i = 0; i < G_N_ELEMENTS(DRIVER_KINDS); i++)
        mfr_driver_list_init(&job.drivers[i], &DRIVER_KINDS[i]);

    if (!glnx_shutil_mkdir_p_at(AT_FDCWD, job.libdir, 0755, NULL, error))
        goto out;

    g_info("Capturing the host graphics stack for %s against %s", arch_tuple, core_mount);
    if (!mfr_capture_job_find_drivers(&job, error))
        goto out;
    loader_manifests = mfr_fetch_loader_manifest_groups();
    argv = mfr_capture_job_make_argv(&job, job.libdir);
    mfr_capture_job_register_patterns(&job, argv);
    mfr_loader_manifest_groups_register_patterns(loader_manifests, argv);
    if (!mfr_capture_run_tool(argv, NULL, error))
        goto out;
    if (!mfr_capture_job_capture_drivers(&job, error))
        goto out;
    if (!mfr_capture_job_install_loader_manifests(&job, loader_manifests, error))
        goto out;
    if (!mfr_capture_job_capture_families(&job, error))
        goto out;
    if (!mfr_capture_job_link_host_glibc_runtime(&job, error))
        goto out;
    if (!mfr_capture_job_link_host_data_dirs(&job, error))
        goto out;
    mfr_capture_job_add_driver_path_vars(&job);

    if (!mfr_capture_job_write_environment(&job, dir, error))
        goto out;
    if (!mfr_capture_job_write_stamp(&job, dir, error))
        goto out;
    ret = TRUE;

out:
    g_free(job.overrides_dir);
    g_free(job.libdir);
    g_free(job.tree_libdir);
    g_free(job.drivers_dir);
    g_free(job.tree_drivers_dir);
    g_hash_table_unref(job.env);
    g_hash_table_unref(job.placed);
    g_ptr_array_unref(job.host_dirs);
    for (size_t i = 0; i < G_N_ELEMENTS(job.drivers); i++)
        mfr_driver_list_clear(&job.drivers[i]);
    return ret;
}

/**
 * mfr_capture_load_environment:
 *
 * Returns: (transfer full): the environment of the capture in @dir, as NAME=value entries
 */
static char **
mfr_capture_load_environment(const char *dir, GError **error)
{
    g_autofree char *path = g_build_filename(dir, ENVIRONMENT_NAME, NULL);
    g_autoptr(sd_json_variant) environment = mf_json_parse_file(path, error);
    g_autoptr(GPtrArray) entries = g_ptr_array_new_with_free_func(g_free);

    if (environment == NULL)
        return NULL;
    if (!sd_json_variant_is_object(environment))
        return glnx_null_throw(error, "%s is not an object", path);

    for (size_t i = 0; i + 1 < sd_json_variant_elements(environment); i += 2) {
        sd_json_variant *key = sd_json_variant_by_index(environment, i);
        sd_json_variant *value = sd_json_variant_by_index(environment, i + 1);

        if (!sd_json_variant_is_string(key) || !sd_json_variant_is_string(value))
            return glnx_null_throw(error, "%s does not map names to strings", path);
        g_ptr_array_add(entries,
                        g_strdup_printf("%s=%s", sd_json_variant_string(key), sd_json_variant_string(value)));
    }
    g_ptr_array_add(entries, NULL);

    return (char **) g_ptr_array_free(g_steal_pointer(&entries), FALSE);
}

/**
 * mfr_capture_new_for_dir:
 *
 * Name the capture in @dir, without loading it. The directory scans use this,
 * as they also see captures that were moved aside, or never completed.
 */
static MfrCapture *
mfr_capture_new_for_dir(const char *dir)
{
    MfrCapture *capture = g_object_new(MFR_TYPE_CAPTURE, NULL);

    capture->dir = g_strdup(dir);
    capture->overrides_dir = g_build_filename(dir, OVERRIDES_NAME, NULL);
    return capture;
}

/**
 * mfr_capture_new:
 * @core_hash: the verity root hash of the core the capture is for
 * @arch_tuple: the multiarch tuple of the applications that run on it
 *
 * Name the capture for a core. Nothing is read or built until
 * mfr_capture_ensure() is called.
 *
 * Returns: (transfer full): the capture
 */
MfrCapture *
mfr_capture_new(const char *core_hash, const char *arch_tuple)
{
    g_autofree char *dir = g_build_filename(MFR_CAPTURE_DIR, core_hash, NULL);
    MfrCapture *capture = mfr_capture_new_for_dir(dir);

    capture->arch_tuple = g_strdup(arch_tuple);
    return capture;
}

/**
 * mfr_capture_unload:
 *
 * Forget what was loaded from the directory of the capture.
 */
static void
mfr_capture_unload(MfrCapture *capture)
{
    g_clear_pointer(&capture->id, g_free);
    g_clear_pointer(&capture->environment, g_strfreev);
    g_clear_pointer(&capture->stamp, sd_json_variant_unref);
}

/**
 * mfr_capture_load:
 *
 * Load the stamp and the environment from the directory of the capture,
 * in place of what was loaded before.
 *
 * Returns: %FALSE if the capture is incomplete, leaving what was loaded as it was
 */
static gboolean
mfr_capture_load(MfrCapture *capture, GError **error)
{
    g_autofree char *stamp_path = g_build_filename(capture->dir, STAMP_NAME, NULL);
    g_autoptr(sd_json_variant) stamp = mf_json_parse_file(stamp_path, error);
    g_auto(GStrv) environment = NULL;
    const char *id;

    if (stamp == NULL)
        return FALSE;
    id = mf_json_get_string(stamp, "id");
    if (id == NULL || !sd_json_variant_is_array(sd_json_variant_by_key(stamp, "dirs")) ||
        !sd_json_variant_is_array(sd_json_variant_by_key(stamp, "targets")))
        return glnx_throw(error, "%s is not a capture stamp", stamp_path);
    environment = mfr_capture_load_environment(capture->dir, error);
    if (environment == NULL)
        return FALSE;

    mfr_capture_unload(capture);
    capture->id = g_strdup(id);
    capture->environment = g_steal_pointer(&environment);
    capture->stamp = g_steal_pointer(&stamp);
    return TRUE;
}

/**
 * mfr_capture_new_from_dir:
 *
 * Load the capture in @dir.
 *
 * Returns: (transfer full): the capture, or %NULL if it is incomplete
 */
static MfrCapture *
mfr_capture_new_from_dir(const char *dir, GError **error)
{
    g_autoptr(MfrCapture) capture = mfr_capture_new_for_dir(dir);

    if (!mfr_capture_load(capture, error))
        return NULL;
    return g_steal_pointer(&capture);
}

/**
 * mfr_stamp_entry_changed:
 *
 * Check whether a file or directory that a stamp records has changed since
 * the capture was made.
 */
static gboolean
mfr_stamp_entry_changed(sd_json_variant *entry)
{
    sd_json_variant *path = sd_json_variant_by_key(entry, "path");
    sd_json_variant *mtime = sd_json_variant_by_key(entry, "mtime");
    sd_json_variant *size = sd_json_variant_by_key(entry, "size");
    struct stat st;

    if (!sd_json_variant_is_string(path) || !sd_json_variant_is_unsigned(mtime))
        return TRUE;
    if (stat(sd_json_variant_string(path), &st) != 0)
        return sd_json_variant_unsigned(mtime) != 0;
    if (mfr_stat_mtime_ns(&st) != sd_json_variant_unsigned(mtime))
        return TRUE;
    if (size != NULL &&
        (!sd_json_variant_is_unsigned(size) || sd_json_variant_unsigned(size) != (guint64) st.st_size))
        return TRUE;
    return FALSE;
}

/**
 * mfr_stamp_find_changed:
 *
 * Find the first changed entry in the @key list of a stamp.
 *
 * Returns: %TRUE with its path in @ret_changed if one changed
 */
static gboolean
mfr_stamp_find_changed(sd_json_variant *stamp, const char *key, const char **ret_changed)
{
    sd_json_variant *entries = sd_json_variant_by_key(stamp, key);

    for (size_t i = 0; i < sd_json_variant_elements(entries); i++) {
        sd_json_variant *entry = sd_json_variant_by_index(entries, i);

        if (mfr_stamp_entry_changed(entry)) {
            sd_json_variant *path = sd_json_variant_by_key(entry, "path");

            *ret_changed = sd_json_variant_is_string(path) ? sd_json_variant_string(path) : "?";
            return TRUE;
        }
    }
    return FALSE;
}

/**
 * mfr_capture_is_fresh:
 *
 * Check whether the library and manifest directories of the host are as they
 * were when the capture was made. We only compare their modification times,
 * which is cheap enough to do for every launch.
 */
gboolean
mfr_capture_is_fresh(const MfrCapture *capture)
{
    const char *changed;

    if (mfr_stamp_find_changed(capture->stamp, "dirs", &changed)) {
        g_info("Capture %s is stale: %s changed", capture->dir, changed);
        return FALSE;
    }
    return TRUE;
}

/**
 * mfr_capture_is_used:
 * @used_ids: (nullable): the IDs of the captures that trees still in use were made with
 *
 * Check whether a tree that is still in use was made with the capture.
 */
static gboolean
mfr_capture_is_used(const MfrCapture *capture, GHashTable *used_ids)
{
    return used_ids != NULL && g_hash_table_contains(used_ids, capture->id);
}

/**
 * mfr_capture_move_aside:
 * @reason: why we do not use the capture anymore, for the log
 *
 * Move a capture that we do not use anymore out of the way of a fresh
 * capture for its core.
 *
 * We never delete a capture while the daemon runs: a tree made with it may
 * have been handed to a runner that has not attached it yet, and no mount
 * table shows such a tree. Running overlays keep working, as they hold the
 * directory, not its path. When the daemon starts, it deletes the captures
 * that nothing uses anymore, see mfr_capture_check_all().
 */
static gboolean
mfr_capture_move_aside(const MfrCapture *capture, const char *reason, GError **error)
{
    g_autofree char *name = g_path_get_basename(capture->dir);
    g_autofree char *stale_dir = NULL;

    if (g_str_has_prefix(name, STALE_PREFIX))
        return TRUE;

    stale_dir = g_strconcat(MFR_CAPTURE_DIR "/", STALE_PREFIX, capture->id, NULL);
    g_info("Moving capture %s aside: %s", name, reason);
    if (rename(capture->dir, stale_dir) != 0)
        return glnx_throw_errno_prefix(error, "Unable to rename %s to %s", capture->dir, stale_dir);
    return TRUE;
}

/**
 * mfr_capture_retire:
 * @used_ids: (nullable): the IDs of the captures that trees still in use were made with
 * @reason: why we do not use the capture anymore, for the log
 *
 * Delete a capture that we do not use anymore, or move it aside if a tree
 * that is still in use was made with it. We only do this when the daemon
 * starts, when no tree can be on its way to a runner.
 */
static gboolean
mfr_capture_retire(const MfrCapture *capture, GHashTable *used_ids, const char *reason, GError **error)
{
    g_autofree char *name = g_path_get_basename(capture->dir);

    if (mfr_capture_is_used(capture, used_ids))
        return mfr_capture_move_aside(capture, reason, error);

    g_info("Removing capture %s: %s", name, reason);
    return glnx_shutil_rm_rf_at(AT_FDCWD, capture->dir, NULL, error);
}

/**
 * mfr_capture_ensure:
 * @core_mount: where the core the capture is for is mounted
 *
 * Make sure the capture exists and matches the current host: load it from its
 * directory if that was not done yet. Build it anew if the capture didn't exist
 * or if the host changed since it was made. A capture that is built anew gets a
 * new ID, see mfr_capture_get_id().
 *
 * Returns: %TRUE if the capture can be used
 */
gboolean
mfr_capture_ensure(MfrCapture *capture, const char *core_mount, GError **error)
{
    g_autofree char *name = g_path_get_basename(capture->dir);
    g_autofree char *tmp_dir = g_strconcat(MFR_CAPTURE_DIR "/", MF_TMP_PREFIX, name, NULL);
    g_autoptr(GError) load_error = NULL;

    g_return_val_if_fail(capture->arch_tuple != NULL, FALSE);

    if (capture->id == NULL && g_file_test(capture->dir, G_FILE_TEST_IS_DIR) &&
        !mfr_capture_load(capture, &load_error)) {
        g_warning("Discarding capture %s: %s", capture->dir, load_error->message);
        if (!glnx_shutil_rm_rf_at(AT_FDCWD, capture->dir, NULL, error))
            return FALSE;
    }
    if (capture->id != NULL) {
        if (mfr_capture_is_fresh(capture))
            return TRUE;
        if (!mfr_capture_move_aside(capture, "the host changed", error))
            return FALSE;
        mfr_capture_unload(capture);
    }

    /* the new capture takes the place of the old one only once it is complete */
    if (!glnx_shutil_rm_rf_at(AT_FDCWD, tmp_dir, NULL, error) ||
        !glnx_shutil_mkdir_p_at(AT_FDCWD, tmp_dir, 0755, NULL, error))
        return FALSE;
    if (!mfr_capture_build(tmp_dir, core_mount, capture->arch_tuple, error)) {
        glnx_shutil_rm_rf_at(AT_FDCWD, tmp_dir, NULL, NULL);
        return FALSE;
    }
    if (rename(tmp_dir, capture->dir) != 0) {
        glnx_throw_errno_prefix(error, "Unable to rename %s to %s", tmp_dir, capture->dir);
        glnx_shutil_rm_rf_at(AT_FDCWD, tmp_dir, NULL, NULL);
        return FALSE;
    }

    return mfr_capture_load(capture, error);
}

/**
 * mfr_capture_check_all:
 * @used_ids: (nullable): the IDs of the captures that trees still in use were made with
 *
 * Retire every capture whose host files changed, and every one that was moved
 * aside, see mfr_capture_retire(), and remove what an interrupted capture left
 * behind. Done when the daemon starts.
 */
void
mfr_capture_check_all(GHashTable *used_ids)
{
    g_autoptr(GDir) entries = g_dir_open(MFR_CAPTURE_DIR, 0, NULL);
    const char *name;

    while (entries != NULL && (name = g_dir_read_name(entries)) != NULL) {
        g_autofree char *dir = g_build_filename(MFR_CAPTURE_DIR, name, NULL);
        g_autofree char *reason = NULL;
        g_autoptr(MfrCapture) capture = NULL;
        g_autoptr(GError) error = NULL;
        const char *changed = NULL;

        if (g_str_has_prefix(name, MF_TMP_PREFIX)) {
            reason = g_strdup("it was not completed");
        } else {
            capture = mfr_capture_new_from_dir(dir, &error);
            if (capture == NULL)
                reason = g_strdup(error->message);
            else if (g_str_has_prefix(name, STALE_PREFIX))
                reason = g_strdup("it was replaced");
            else if (mfr_stamp_find_changed(capture->stamp, "dirs", &changed) ||
                     mfr_stamp_find_changed(capture->stamp, "targets", &changed))
                reason = g_strdup_printf("the host changed: %s", changed);
        }
        if (reason == NULL)
            continue;

        g_clear_error(&error);
        if (capture != NULL) {
            if (!mfr_capture_retire(capture, used_ids, reason, &error))
                g_warning("%s", error->message);
            continue;
        }
        g_info("Removing capture %s: %s", name, reason);
        if (!glnx_shutil_rm_rf_at(AT_FDCWD, dir, NULL, &error))
            g_warning("%s", error->message);
    }
}

/**
 * mfr_capture_discard_all:
 *
 * Move every capture aside, so the next launch captures again.
 */
gboolean
mfr_capture_discard_all(GError **error)
{
    g_autoptr(GDir) entries = g_dir_open(MFR_CAPTURE_DIR, 0, NULL);
    const char *name;

    while (entries != NULL && (name = g_dir_read_name(entries)) != NULL) {
        g_autofree char *dir = g_build_filename(MFR_CAPTURE_DIR, name, NULL);
        g_autoptr(MfrCapture) capture = NULL;

        if (!g_str_has_prefix(name, MF_TMP_PREFIX))
            capture = mfr_capture_new_from_dir(dir, NULL);
        if (capture != NULL) {
            if (!mfr_capture_move_aside(capture, "a fresh capture was requested", error))
                return FALSE;
            continue;
        }
        g_info("Removing capture %s", name);
        if (!glnx_shutil_rm_rf_at(AT_FDCWD, dir, NULL, error))
            return FALSE;
    }
    return TRUE;
}

const char *
mfr_capture_get_id(const MfrCapture *capture)
{
    return capture->id;
}

const char *
mfr_capture_get_overrides_dir(const MfrCapture *capture)
{
    return capture->overrides_dir;
}

const char *const *
mfr_capture_get_environment(const MfrCapture *capture)
{
    return (const char *const *) capture->environment;
}
