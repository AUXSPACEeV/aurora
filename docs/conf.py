# Configuration file for the Sphinx documentation builder.
#
# For the full list of built-in configuration values, see the documentation:
# https://www.sphinx-doc.org/en/master/usage/configuration.html

# -- Project information -----------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#project-information

project = 'AURORA'
copyright = '2025, Auxspace e.V'
author = 'Maximilian Stephan'
release = '2025'

# -- General configuration ---------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#general-configuration

extensions = [
    'myst_parser',
    'sphinxcontrib.mermaid',
    'sphinx_design',
    'sphinx_copybutton',
    'sphinx_favicon',
    'notfound.extension',
]
myst_enable_extensions = ['colon_fence']

templates_path = ['_templates']
exclude_patterns = ['_build', 'Thumbs.db', '.DS_Store', '.venv']

# -- Options for HTML output -------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#options-for-html-output

html_theme = 'sphinx_rtd_theme'
html_static_path = ['_static']
# TODO: Replace when AURORA icon design is done
html_logo = '_static/logo/AUXSPACE_Logo_weiss-1024x159.png'
html_theme_options = {
    'logo_only': True,
}

# -- Sphinx Favicon ----------------------------------------------------------

favicons = [
    {'href': 'favicon/cropped-Logo_signet_blau_quadrat-1-32x32.png'},
    {'href': 'favicon/cropped-Logo_signet_blau_quadrat-1-192x192.png'},
    {
        'rel': 'apple-touch-icon',
        'href': 'favicon/cropped-Logo_signet_blau_quadrat-1-180x180.png',
    },
    {
        'rel': 'msapplication-TileImage',
        'href': 'favicon/cropped-Logo_signet_blau_quadrat-1-270x270.png',
    }
]

# -- Sphinx Notfound Page ----------------------------------------------------

notfound_template = '404.rst'
