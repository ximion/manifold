#
# libmanifold: The client library of Manifold
#

mf_lib_inc = include_directories('.')

mflib_src = [
    'mf-client.c',
    'mf-component.c',
    'mf-error.c',
    'mf-interface.c',
    'mf-progress.c',
    'mf-result.c',
    'mf-version.c',
]

mflib_pub_headers = [
    'manifold.h',
    'mf-client.h',
    'mf-component.h',
    'mf-error.h',
    'mf-progress.h',
    'mf-result.h',
]

mflib_priv_headers = [
    'mf-component-private.h',
    'mf-interface-private.h',
    'mf-macros-private.h',
    'mf-progress-private.h',
    'mf-result-private.h',
]

version_parts = mf_version.split('.')
version_conf = configuration_data()
version_conf.set('MF_MAJOR_VERSION_CONF', version_parts[0])
version_conf.set('MF_MINOR_VERSION_CONF', version_parts[1])
version_conf.set('MF_MICRO_VERSION_CONF', version_parts[2])

mf_version_h = configure_file(
    input: 'mf-version.h.in',
    output: 'mf-version.h',
    install_dir: get_option('includedir') / 'manifold',
    configuration: version_conf,
)

manifold_lib = library('manifold',
    [mflib_src,
     mflib_pub_headers,
     mflib_priv_headers,
     mf_version_h],
    soversion: mf_api_level,
    version: mf_version,
    dependencies: [
        glib_dep,
        gobject_dep,
        gio_dep,
        libsystemd_dep
    ],
    include_directories: [root_inc_dir],
    install: true,
)

install_headers(mflib_pub_headers, subdir: 'manifold')

manifold_dep = declare_dependency(
    link_with: manifold_lib,
    sources: [mf_version_h],
    dependencies: [
        glib_dep,
        gobject_dep,
        gio_dep
    ],
    include_directories: [mf_lib_inc],
)

pkgc.generate(
    manifold_lib,
    version: mf_version,
    name: 'manifold',
    description: 'Library to install and manage Manifold components.',
    filebase: 'manifold',
    subdirs: 'manifold',
    requires: ['glib-2.0', 'gobject-2.0', 'gio-2.0'],
)
