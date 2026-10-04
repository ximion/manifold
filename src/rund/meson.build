#
# manifold-rund: Service composing mount trees and managing the runtime lifecycle
#

mf_rund_inc_dir = include_directories('.')

mf_rund_src = [
    'mfr-capture.c',
    'mfr-capture.h',
    'mfr-composer.c',
    'mfr-composer.h',
    'mfr-app.c',
    'mfr-app.h',
    'mfr-image-mounts.c',
    'mfr-image-mounts.h',
    'mfr-run-interface.c',
    'mfr-run-interface.h',
    'mfr-service.c',
    'mfr-service.h',
    'mfr-tree.c',
    'mfr-tree.h',
]

manifold_rund_lib = static_library('manifold-rund',
    [mf_rund_src,
     mf_daemon_shared_src],
    c_args: hardening_c_args + ['-DG_LOG_DOMAIN="manifold-rund"'],
    include_directories: [root_inc_dir, shared_inc_dir],
    dependencies: [
        manifold_dep,
        glib_dep,
        gobject_dep,
        libsystemd_dep,
        libglnx_dep,
        libcryptsetup_dep
    ],
)

manifold_rund_static_dep = declare_dependency(
    link_with: manifold_rund_lib,
    include_directories: [include_directories('.'), shared_inc_dir],
    dependencies: [
        manifold_dep,
        glib_dep,
        gobject_dep,
        libsystemd_dep,
        libglnx_dep
    ],
)

manifold_rund_exe = executable('manifold-rund',
    ['manifold-rund.c'],
    c_args: hardening_c_args + ['-DG_LOG_DOMAIN="manifold-rund"'],
    link_args: hardening_link_args,
    include_directories: [root_inc_dir, shared_inc_dir],
    dependencies: [
        manifold_rund_static_dep,
        libcryptsetup_dep
    ],
    install: true,
    install_dir: get_option('libexecdir'),
)
