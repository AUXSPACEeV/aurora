# Configuration file for the Sphinx documentation builder.
#
# For the full list of built-in configuration values, see the documentation:
# https://www.sphinx-doc.org/en/master/usage/configuration.html

# -- Project information -----------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#project-information

project = 'AURORA'
copyright = '2025-2026, Auxspace e.V.'
author = 'Auxspace e.V.'
release = '1.1.0'

# -- General configuration ---------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#general-configuration

extensions = [
    'sphinx.ext.intersphinx',
    'breathe',
    'myst_parser',
]

source_suffix = {
    '.rst': 'restructuredtext',
    '.md': 'markdown',
}

myst_enable_extensions = [
    'colon_fence',
    'fieldlist',
]

templates_path = ['_templates']
exclude_patterns = ['_build_sphinx', '_build_doxygen', 'Thumbs.db',
                    '.DS_Store', 'README.md', '_doxygen/main.md']

# -- Options for HTML output -------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#options-for-html-output

html_theme = 'furo'
html_logo = 'img/logo.png'
html_title = 'Auxspace e.V. <b>AURORA</b>'

html_theme_options = {
    # White and blue colour scheme
    "light_css_variables": {
        "color-brand-primary": "#1a5fb4",
        "color-brand-content": "#1a5fb4",
        "color-sidebar-brand-text": "#1a5fb4",
        "color-sidebar-link-text--top-level": "#1a5fb4",
        "color-sidebar-link-text": "#333333",
        "color-sidebar-background": "#f8f9fa",
        "color-sidebar-background-border": "#dce0e5",
        "color-admonition-title--note": "#1a5fb4",
        "color-admonition-title-background--note": "#e8f0fe",
    },
    "dark_css_variables": {
        "color-brand-primary": "#6ea8fe",
        "color-brand-content": "#6ea8fe",
        # Lighter dark background for main content
        "color-background-primary": "#1e2028",
        "color-background-secondary": "#252830",
        "color-background-border": "#3a3d46",
        "color-foreground-primary": "#dcdee3",
        "color-foreground-border": "#505460",
        # Dark sidebar
        "color-sidebar-background": "#111318",
        "color-sidebar-background-border": "#1a1d24",
        "color-sidebar-brand-text": "#6ea8fe",
        "color-sidebar-link-text--top-level": "#6ea8fe",
        "color-sidebar-link-text": "#b0b4be",
        "color-sidebar-caption-text": "#8890a0",
    },
    "sidebar_hide_name": False,
    "navigation_with_keys": True,
    "top_of_page_buttons": ["view"],
}

html_static_path = ['_static']
html_css_files = ['custom.css']

# -- Options for Intersphinx -------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/extensions/intersphinx.html

intersphinx_mapping = {'zephyr': ('https://docs.zephyrproject.org/latest/', None)}

# -- Options for Breathe -----------------------------------------------------
# https://breathe.readthedocs.io/en/latest/

breathe_projects = {'aurora': '_build_doxygen/xml'}
breathe_default_project = 'aurora'
