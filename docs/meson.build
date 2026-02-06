#
# Documentation
#
sphinx_build = find_program('sphinx-build', required: get_option('docs'))
if sphinx_build.found()
    run_target('docs',
        command: [sphinx_build,
                  '-b', 'html',
                  '-W', '--keep-going',
                  '-D', 'release=' + mf_version,
                  meson.current_source_dir(),
                  meson.current_build_dir() / 'html'],
    )
endif
