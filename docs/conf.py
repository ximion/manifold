# Configuration of the Sphinx documentation of Manifold.
project = 'Manifold'
author = 'Matthias Klumpp'
copyright = '2025-2026, Matthias Klumpp'

extensions = []

exclude_patterns = ['_build']

html_theme = 'sphinx_book_theme'
html_title = 'Manifold'
html_theme_options = {
    'repository_url': 'https://github.com/ximion/manifold',
    'use_repository_button': True,
    'show_toc_level': 2,
}
