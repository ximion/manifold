# Build only capsule-capture-libs from the libcapsule submodule

libelf_dep = dependency('libelf', version: '>= 0.152')
libdl_dep = cc.find_library('dl', required: false)

capsule_version = '0.20251201'
capsule_c_args = [
    '-D_GNU_SOURCE',
    '-DHOST_PREFIX=""',
    '-DPACKAGE_TARNAME="libcapsule"',
    '-DPACKAGE_VERSION="@0@"'.format(capsule_version),
    '-DHAVE_SECURE_GETENV=1',
    '-fvisibility=hidden',
]
if cc.has_header('stdalign.h')
    capsule_c_args += '-DHAVE_STDALIGN_H=1'
endif
# match libcapsule's own warning setup
capsule_c_args += cc.get_supported_arguments([
    '-Wno-pointer-arith',
    '-Wno-declaration-after-statement',
])

executable('capsule-capture-libs',
    files(
        'libcapsule/utils/capture-libs.c',
        'libcapsule/utils/library-cmp.c',
        'libcapsule/utils/tools.c',
        'libcapsule/utils/utils.c',
        'libcapsule/utils/ld-cache.c',
        'libcapsule/utils/ld-libs.c',
    ),
    c_args: capsule_c_args,
    dependencies: [libelf_dep, libdl_dep],
    install: true,
    install_dir: mf_libdir / 'capsule',
)
