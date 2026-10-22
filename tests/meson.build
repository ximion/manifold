#
# Tests
#

python_exe = import('python').find_installation('python3')

# helpers the tests share
mft_test_utils_lib = static_library('mft-test-utils',
    ['mft-utils.c',
     'mft-utils.h',
     'mft-sandbox.c',
     'mft-sandbox.h',
    ],
    include_directories: [root_inc_dir],
    dependencies: [
        glib_dep,
        libglnx_dep,
        manifold_dep.partial_dependency(includes: true, sources: true)
    ],
)
mft_test_utils_dep = declare_dependency(
    link_with: mft_test_utils_lib,
    dependencies: [glib_dep, libglnx_dep],
)

test_sandbox_exe = executable('mf-test-sandbox',
    ['mft-sandbox-main.c'],
    include_directories: [root_inc_dir],
    dependencies: [mft_test_utils_dep],
)

#
# Unit tests
#
test('utils',
    executable('test-utils',
        ['test-utils.c'],
        include_directories: [root_inc_dir],
        dependencies: [manifold_dep],
    ),
    suite: 'unit',
)

test('bundle',
    executable('test-bundle',
        ['test-bundle.c'],
        include_directories: [root_inc_dir],
        dependencies: [
            manifold_dep,
            mft_test_utils_dep,
            libzip_dep,
            blake3_dep,
        ],
    ),
    suite: 'unit',
)

test('store-read',
    executable('test-store-read',
        ['test-store-read.c'],
        include_directories: [root_inc_dir],
        dependencies: [manifold_dep, mft_test_utils_dep],
    ),
    suite: 'unit',
)

#
# Unit tests that need a sandbox
#
test('store',
    executable('test-store',
        ['test-store.c'],
        include_directories: [root_inc_dir],
        dependencies: [manifoldd_static_dep, mft_test_utils_dep],
    ),
    suite: 'unit',
)

test('compose',
    executable('test-run-compose',
        ['test-compose.c'],
        include_directories: [root_inc_dir],
        dependencies: [manifold_rund_static_dep, mft_test_utils_dep],
    ),
    suite: 'unit',
)

test('capture',
    executable('test-capture',
        ['test-capture.c'],
        include_directories: [root_inc_dir],
        dependencies: [manifold_rund_static_dep, mft_test_utils_dep],
    ),
    suite: 'unit',
)

#
# End-to-end
#

# skipped without b3sum and varlinkctl, which the test needs
test('installer',
    test_sandbox_exe,
    args: [python_exe.full_path(), files('test-installer.py'),
           '--daemon', manifoldd_exe.full_path(), '--client', manifold_exe.full_path()],
    depends: [manifoldd_exe, manifold_exe],
    suite: 'e2e',
    timeout: 120,
)

# enters the sandbox itself, for one of its halves
test('runner',
    python_exe,
    args: [files('test-runner.py'), '--sandbox', test_sandbox_exe.full_path(), '--runner', manifold_run_exe.full_path()],
    depends: [test_sandbox_exe, manifold_run_exe],
    suite: 'e2e',
    timeout: 60,
)
