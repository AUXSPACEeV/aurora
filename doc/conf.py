# Configuration file for the Sphinx documentation builder.
#
# For the full list of built-in configuration values, see the documentation:
# https://www.sphinx-doc.org/en/master/usage/configuration.html

# -- Project information -----------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#project-information

project = 'AURORA'
copyright = '2024, Auxspace e.V.'
author = 'Auxspace e.V.'
release = '1.1.0'

# -- General configuration ---------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#general-configuration

extensions = [
    'sphinx.ext.intersphinx',
    'breathe',
]

templates_path = ['_templates']
exclude_patterns = ['_build_sphinx', '_build_doxygen', 'Thumbs.db', '.DS_Store']

# -- Options for HTML output -------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#options-for-html-output

html_theme = 'alabaster'
html_logo = 'img/logo.jpeg'
html_theme_options = {
    'description': 'A Zephyr-based avionics stack for the METER-2 rocket',
    'github_user': 'AUXSPACEeV',
    'github_repo': 'aurora',
    'github_button': True,
}

# -- Options for Intersphinx -------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/extensions/intersphinx.html

intersphinx_mapping = {'zephyr': ('https://docs.zephyrproject.org/latest/', None)}

# -- Options for Breathe -----------------------------------------------------
# https://breathe.readthedocs.io/en/latest/

breathe_projects = {'aurora': '_build_doxygen/xml'}
breathe_default_project = 'aurora'
