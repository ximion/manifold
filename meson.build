project('Manifold', 'c',
  meson_version: '>=0.62',
  default_options: ['c_std=gnu23', 'b_pie=true'],

  license: 'LGPL-2.1-or-later',
  version: '0.0.1',
)

cc = meson.get_compiler('c')
source_root = meson.project_source_root()

mf_version = meson.project_version()
mf_api_level = '0'

root_inc_dir = include_directories ('.')

mf_libdir = get_option('libdir') / 'manifold'

#
# Custom C flags
#

if cc.get_id() == 'msvc'
  error('MSVC is not supported as it does not support modern C11 features and __attribute__((cleanup))')
endif

sanitizer_libs = []
if get_option('maintainer')
    maintainer_c_args = [
      '-Werror',
      '-Wall',
      '-Wextra',
      '-Wcast-align',
      '-Wno-uninitialized',
      '-Wempty-body',
      '-Winit-self',
      '-Wnull-dereference',
      '-Wfloat-equal',
      '-Wno-error=comment',
      '-Werror=format-security',
    ]
    add_project_arguments(maintainer_c_args, language: 'c')
endif

# a few compiler warning flags we always want enabled
add_project_arguments(
  cc.get_supported_arguments([
    '-Werror=shadow',
    '-Werror=empty-body',
    '-Werror=strict-prototypes',
    '-Werror=missing-prototypes',
    '-Werror=implicit-function-declaration',
    '-Werror=pointer-arith',
    '-Werror=missing-declarations',
    '-Werror=return-type',
    '-Werror=int-conversion',
    '-Werror=incompatible-pointer-types',
    '-Werror=misleading-indentation',
    '-Werror=missing-include-dirs',
    '-Werror=format-security',

    '-Wno-missing-field-initializers',
    '-Wno-error=missing-field-initializers',
    '-Wno-unused-parameter',
    '-Wno-error=unused-parameter',
  ]),
  language: 'c'
)

add_project_arguments('-DMF_COMPILATION', language: 'c')

#
# Hardening flags we want to enforce on some binaries
#
hardening_c_args = cc.get_supported_arguments([
    '-fstack-protector-strong',
    '-fstack-clash-protection',
    '-fcf-protection=full',
    '-ftrivial-auto-var-init=zero',
])
if get_option('optimization') != '0'
    # fortification requires optimization
    hardening_c_args += ['-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=3']
endif

hardening_link_args = cc.get_supported_link_arguments([
    '-Wl,-z,relro',
    '-Wl,-z,now',
    '-Wl,-z,noexecstack',
])

#
# Dependencies
#
curl_dep = dependency('libcurl', version: '>= 7.62')
appstream_dep = dependency('appstream')
libsystemd_dep = dependency('libsystemd', version: '>= 257')
glib_dep = dependency('glib-2.0', version: '>= 2.80')
gobject_dep = dependency('gobject-2.0', version: '>= 2.80')
gio_dep = dependency('gio-2.0', version: '>= 2.80')
systemd_dep = dependency('systemd')
libzip_dep = dependency('libzip', version: '>= 1.7')
libglnx_dep = dependency('libglnx', default_options: ['tests=false'])
libcryptsetup_dep = dependency('libcryptsetup', version: '>= 2.4')

#
# Modules
#
fs   = import('fs')
pkgc = import('pkgconfig')

#
# Directories
#
mf_prefix     = get_option('prefix')
mf_bindir     = mf_prefix / get_option('bindir')
mf_libexecdir = mf_prefix / get_option('libexecdir')
mf_runtimedir = '/run/manifold'
mf_store_dir  = '/var/lib/manifold'
mf_cache_dir  = '/var/cache/manifold'

# what manifold-rund mounts, in a mount namespace of its own, and what it caches
mf_run_mount_dir = mf_runtimedir / 'rund'
mf_run_cache_dir = mf_cache_dir / 'run'
mf_capsule_bin_dir = mf_prefix / mf_libdir / 'capsule'

systemd_system_unit_dir = systemd_dep.get_variable(
    pkgconfig: 'systemdsystemunitdir',
    pkgconfig_define: ['prefix', mf_prefix],
)

#
# Varlink
#
mf_interface     = 'org.freedesktop.manifold'
mf_socket        = mf_runtimedir / mf_interface
mf_run_interface = 'org.freedesktop.manifold.run'
mf_run_socket    = mf_runtimedir / mf_run_interface

#
# Configuration
#
conf = configuration_data()
conf.set_quoted('MF_VERSION', mf_version)
conf.set_quoted('MF_RUN_INTERFACE', mf_run_interface)
conf.set_quoted('MF_RUN_SOCKET_PATH', mf_run_socket)
conf.set_quoted('MF_INTERFACE', mf_interface)
conf.set_quoted('MF_SOCKET_PATH', mf_socket)
conf.set_quoted('MF_RUNTIME_DIR', mf_runtimedir)
conf.set_quoted('MF_STORE_DIR', mf_store_dir)
conf.set_quoted('MF_CACHE_DIR', mf_cache_dir)
conf.set_quoted('MF_RUN_MOUNT_DIR', mf_run_mount_dir)
conf.set_quoted('MF_RUN_CACHE_DIR', mf_run_cache_dir)
conf.set_quoted('MF_CAPSULE_BIN_DIR', mf_capsule_bin_dir)
conf.set_quoted('GETTEXT_PACKAGE', 'manifold')
conf.set_quoted('LOCALEDIR',
                get_option('prefix') / get_option('localedir'))
conf.set_quoted('LOCALSTATEDIR',
                get_option('prefix') / get_option('localstatedir'))
conf.set_quoted('LIBEXECDIR',
                get_option('prefix') / get_option('libexecdir'))
conf.set_quoted('LIBDIR',
                get_option('prefix') / get_option('libdir'))
configure_file(output: 'config.h', configuration: conf)

#
# Subdirectories
#
subdir('capsule')
subdir('data')
subdir('src')
subdir('tests')
subdir('docs')
