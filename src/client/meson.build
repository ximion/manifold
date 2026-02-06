#
# The Manifold command-line client
#

manifold_client_src = [
    'manifold.c',
    'mfc-context.c',
    'mfc-context.h',
    'mfc-manage.c',
    'mfc-manage.h',
    'mfc-query.c',
    'mfc-query.h',
    'mfc-progress-bar.c',
    'mfc-progress-bar.h',
    'mfc-util.c',
    'mfc-util.h'
]

manifold_exe = executable('manifold',
    [manifold_client_src],
    include_directories: [root_inc_dir],
    dependencies: [
        manifold_dep,
        glib_dep,
    ],
    install: true,
    install_dir: get_option('bindir'),
)
