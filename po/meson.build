#
# Translations
#

mf_gettext_domain = 'manifold'
i18n.gettext(mf_gettext_domain,
    preset: 'glib',
    args: [
        '--default-domain=' + mf_gettext_domain,
        '--from-code=UTF-8',
        '-i', '-F', '-c', '--no-wrap',
        '--package-name=' + mf_gettext_domain,
        '--copyright-holder=Matthias Klumpp',
        '--msgid-bugs-address=https://github.com/ximion/manifold/issues',
    ],
)

run_target('make-linguas',
    command: ['sh', meson.current_source_dir() / 'update-linguas.sh'],
)

# maintainer shortcut for updating l10n data
run_target('l10n-update',
    command: [
        'ninja', '-C', meson.project_build_root(),
        'make-linguas',
        mf_gettext_domain + '-pot',
    ],
)
