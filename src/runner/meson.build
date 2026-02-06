#
# manifold-run: The unprivileged, user-facing runner
#
manifold_run_exe = executable('manifold-run',
    ['manifold-run.c'],
    include_directories: [root_inc_dir],
    dependencies: [libsystemd_dep],
    install: true,
    install_dir: get_option('bindir'),
)
