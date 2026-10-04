#
# The Manifold component management service
#

manifoldd_src = [
    'mfd-manager.c',
    'mfd-manager.h',
    'mfd-policy.c',
    'mfd-policy.h',
    'mfd-service.c',
    'mfd-service.h',
    'mfd-store-writer.c',
    'mfd-store-writer.h',
    'mfd-utils.c',
    'mfd-utils.h',
]

manifoldd_c_args = hardening_c_args + ['-DG_LOG_DOMAIN="manifoldd"']

manifoldd_lib = static_library('manifoldd',
    [manifoldd_src, mf_daemon_shared_src],
    c_args: manifoldd_c_args,
    include_directories: [root_inc_dir, shared_inc_dir],
    dependencies: [
        manifold_dep,
        glib_dep,
        libsystemd_dep,
        libzip_dep,
        appstream_dep,
        libglnx_dep,
    ],
)

manifoldd_static_dep = declare_dependency(
    link_with: manifoldd_lib,
    include_directories: [include_directories('.'), shared_inc_dir],
    dependencies: [
        manifold_dep,
        glib_dep,
        libsystemd_dep,
        libglnx_dep,
    ],
)

manifoldd_exe = executable('manifoldd',
    ['manifoldd.c'],
    c_args: manifoldd_c_args,
    link_args: hardening_link_args,
    include_directories: [root_inc_dir, shared_inc_dir],
    dependencies: [manifoldd_static_dep],
    install: true,
    install_dir: get_option('libexecdir'),
)
