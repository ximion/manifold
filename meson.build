project('Manifold', 'c',
  meson_version: '>=0.62',
  default_options: ['c_std=gnu17'],

  license: 'LGPL-2.1-or-later',
  version: '0.0.1',
)

cc = meson.get_compiler('c')
source_root = meson.project_source_root()

mfld_version = meson.project_version()
mfld_api_level = '0'

root_inc_dir = include_directories ('.')

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

      # used in rsvg's headers, so we can not make this fatal yet
      '-Wno-error=expansion-to-defined',
    ]
    add_project_arguments(maintainer_c_args, language: 'c')
    add_project_arguments(maintainer_c_args, language: 'cpp')
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
    '-Werror=declaration-after-statement',
    '-Werror=format-security',

    '-Wno-missing-field-initializers',
    '-Wno-error=missing-field-initializers',
    '-Wno-unused-parameter',
    '-Wno-error=unused-parameter',
  ]),
  language: 'c'
)

add_project_arguments('-DMFLD_COMPILATION', language: 'c')

#
# Dependencies
#
curl_dep = dependency('libcurl', version: '>= 7.62')
appstream_dep = dependency('appstream')
libsystemd_dep = dependency('libsystemd')

#
# Modules
#
fs   = import('fs')
pkgc = import('pkgconfig')

#
# Directories
#
