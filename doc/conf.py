# Configuration file for the Sphinx documentation builder.
#
# For the full list of built-in configuration values, see the documentation:
# https://www.sphinx-doc.org/en/master/usage/configuration.html

import os
import re
import subprocess
import sys
from pathlib import Path
from sphinx.util import logging as _sphinx_logging

# Resolve paths relative to this workspace (aurora/doc/../.. = west workspace root).
_WORKSPACE = Path(__file__).parents[2]
_ZEPHYR_BASE = _WORKSPACE / "zephyr"

_logger = _sphinx_logging.getLogger(__name__)

# Local extensions (aurora_compat, etc.)
sys.path.insert(0, str(Path(__file__).parent / "_extensions"))

# Expose Zephyr's extension and script directories so that zephyr.domain
# (and its transitive imports) can be loaded without a full Zephyr doc build.
sys.path.insert(0, str(_ZEPHYR_BASE / "doc" / "_extensions"))
sys.path.insert(0, str(_ZEPHYR_BASE / "doc" / "_scripts"))
sys.path.insert(0, str(_ZEPHYR_BASE / "scripts"))
sys.path.insert(0, str(_ZEPHYR_BASE / "scripts" / "west_commands"))
sys.path.insert(0, str(_ZEPHYR_BASE / "scripts" / "dts" / "python-devicetree" / "src"))

# -- Project information -----------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#project-information

project = 'AURORA'
copyright = '2025-2026, Auxspace e.V.'
author = 'Auxspace e.V.'
release = '1.6.1'  # documentation version, NOT project version!

try:
    git_tag = subprocess.check_output(
        ['git', 'describe', '--tags', '--always'],
        cwd=os.path.dirname(__file__),
        stderr=subprocess.DEVNULL,
    ).decode().strip()
except Exception as e:
    print("Could not determine git tag: ", e, file=sys.stderr)
    git_tag = release

version = git_tag  # version that gets displayed in the "version" field of docs
rst_epilog = f'.. |git_tag| replace:: {git_tag}\n'


# -- General configuration ---------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#general-configuration

extensions = [
    'sphinx.ext.intersphinx',
    'breathe',
    'myst_parser',
    'zephyr.domain',
    'zephyr.link-roles',  # registers the zephyr_file role used by board-supported-hw
    'aurora_compat',  # must come after zephyr.domain; fixes missing config values and restores breathe's doxygengroup
    'sphinx_simplepdf',  # WeasyPrint-based PDF builder (`make simplepdf`)
]

source_suffix = {
    '.rst': 'restructuredtext',
    '.md': 'markdown',
}

myst_enable_extensions = [
    'colon_fence',
    'fieldlist',
    'dollarmath',
    'amsmath'
]
myst_heading_anchors = 3

templates_path = ['_templates']
exclude_patterns = ['_build_sphinx', '_build_doxygen', 'Thumbs.db',
                    '.DS_Store', 'README.md', '_doxygen/main.md', "drawio_src"]

# Suppress cross-reference warnings for Zephyr DT binding docs that only
# exist in a full Zephyr documentation build.
suppress_warnings = ['ref.dtcompatible', 'duplicate_declaration.cpp']

# -- Options for PDF output (sphinx-simplepdf, WeasyPrint) -------------------
# https://sphinx-simplepdf.readthedocs.io/

simplepdf_vars = {
    # Brand palette mirrors the Furo light theme (color-brand-primary).
    'primary':        '#1a5fb4',
    'primary-opaque': 'rgba(26, 95, 180, 0.5)',
    'secondary':      '#6ea8fe',
    'links':          '#1a5fb4',
    'cover-bg':       'linear-gradient(135deg, #1a5fb4 0%, #0d3a72 100%)',
    'cover-overlay':  'rgba(0, 0, 0, 0)',
    'top-right-content':  'string(heading)',
    'bottom-right-content': 'counter(page)',
    'top-left-content': 'AURORA',
}
simplepdf_file_name = 'aurora.pdf'

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

# -- Zephyr GitHub link settings ---------------------------------------------
# Drives the "Browse board sources" button inserted by zephyr.domain's
# ConvertBoardNode transform. The URL is built as:
#   {gh_link_base_url}/blob/{gh_link_version}/{prefix}/{docname}
# where prefix is looked up from gh_link_prefixes by matching docname.

_aurora_repo = str(_WORKSPACE / "aurora")
try:
    # Prefer an exact tag (e.g. v1.1.0) for release builds.
    _gh_version = subprocess.check_output(
        ["git", "describe", "--tags", "--exact-match"],
        cwd=_aurora_repo, stderr=subprocess.DEVNULL,
    ).decode().strip()
except subprocess.CalledProcessError:
    _gh_version = subprocess.check_output(
        ["git", "rev-parse", "--abbrev-ref", "HEAD"],
        cwd=_aurora_repo,
    ).decode().strip()

gh_link_base_url = "https://github.com/AUXSPACEeV/aurora"
gh_link_version  = _gh_version
gh_link_prefixes = {
    # Board doc pages are reached via the doc/boards/ symlink; their path
    # in the repo is boards/... with no extra prefix.
    "boards/.*": "",
    # Everything else lives under doc/ in the repo.
    ".*": "doc",
}

# -- Repo / release links in the Furo chrome ---------------------------------
# Two additions modelled on the Zephyr docs chrome:
#   1. An "Open on GitHub" link in the per-page top bar.  Furo renders
#      components/view-this-page.html into the content-icon-container when
#      "view" is listed in top_of_page_buttons (see html_theme_options above);
#      _templates/components/view-this-page.html overrides it to render a
#      labelled GitHub link using the per-page URL computed by
#      _add_aurora_github_url() below.
#   2. A "Releases" entry pinned to the bottom of the sidebar, rendered by
#      _templates/sidebar/aurora-version.html and wired in via html_sidebars.
#
# source_repository/branch are required so that basic-ng's view component
# considers a view link "available" (which gates the link_available block our
# override fills); the URL itself is recomputed per page to also cover board
# doc pages reached through the doc/boards/ symlink.
html_theme_options["source_repository"] = gh_link_base_url + "/"
html_theme_options["source_branch"]     = gh_link_version
html_theme_options["source_directory"]  = "doc/"

# Consumed by _templates/sidebar/aurora-version.html.  The release list is
# maintained by hand -- add each new release tag to the TOP (newest first); the
# sidebar dropdown turns every entry into a link to its GitHub release page
# ({gh_link_base_url}/releases/tag/<tag>).
_AURORA_RELEASES = [
    "v0.5.0",
    "v0.4.2",
    "v0.3.2",
]
html_context = {
    "aurora_releases": _AURORA_RELEASES,
    "aurora_releases_url": gh_link_base_url + "/releases",
    "aurora_releases_tag_base": gh_link_base_url + "/releases/tag",
    # Site-root-relative path to the PDF build of the docs (produced by
    # `make simplepdf` / sphinx-simplepdf, see simplepdf_file_name).  The HTML
    # build does not generate it, so the docs deploy must copy the simplepdf
    # output to this location; the sidebar "Downloads > PDF" link resolves it
    # relative to each page via pathto().
    "aurora_pdf_path": simplepdf_file_name,
}

# Pin the "Releases" entry to the bottom of the sidebar by appending it after
# sidebar/scroll-end.html: everything after the scroll container is rendered
# outside the scroll area, so it sticks to the bottom.  Setting html_sidebars
# replaces Furo's defaults wholesale, so the default sections (copied from
# Furo's theme.conf) must be repeated here.
html_sidebars = {
    "**": [
        "sidebar/brand.html",
        "sidebar/search.html",
        "sidebar/scroll-start.html",
        "sidebar/navigation.html",
        "sidebar/ethical-ads.html",
        "sidebar/scroll-end.html",
        "sidebar/aurora-version.html",
        "sidebar/variant-selector.html",
    ]
}


def _add_aurora_github_url(app, pagename, templatename, context, doctree):
    """Expose a correct per-page GitHub "blob" URL as ``aurora_github_url``.

    basic-ng's built-in view link assumes every page lives under doc/, but
    board doc pages are reached through the doc/boards -> ../boards symlink and
    live at boards/... in the repo (mirroring gh_link_prefixes).  Compute the
    URL here so the top-bar "Open on GitHub" link resolves for both.
    """
    try:
        src_rel = os.path.relpath(app.env.doc2path(pagename), app.srcdir)
    except Exception:
        context["aurora_github_url"] = gh_link_base_url
        return
    src_rel = src_rel.replace(os.sep, "/")
    # boards/* map to the repo root; everything else lives under doc/.
    repo_path = src_rel if src_rel.startswith("boards/") else "doc/" + src_rel
    context["aurora_github_url"] = (
        f"{gh_link_base_url}/blob/{gh_link_version}/{repo_path}"
    )

# -- Zephyr domain tweaks ----------------------------------------------------
# gen_boards_catalog.guess_image makes board image paths relative to ZEPHYR_BASE,
# which fails for Aurora's own boards that live outside the Zephyr tree.
# Patch it to return a srcdir-relative path via the doc/boards/ symlink instead.
import gen_boards_catalog as _gbc

_AURORA_BOARDS = _WORKSPACE / "aurora" / "boards"
_orig_guess_image = _gbc.guess_image

def _guess_image_safe(board_or_shield):
    try:
        return _orig_guess_image(board_or_shield)
    except ValueError:
        # Re-run the same filename search but express the result relative to
        # the Sphinx srcdir using the doc/boards -> ../boards symlink.
        name = board_or_shield.name
        parts = name.split("_")
        patterns = (
            *[f"**/{('_'.join(parts[:i]))}.{{ext}}" for i in range(len(parts) - 1, 0, -1)],
            "**/*{name}*.{ext}",
            "**/*.{ext}",
        )
        img = _gbc.guess_file_from_patterns(
            board_or_shield.dir, patterns, name, ("webp", "png", "jpg", "jpeg")
        )
        if img is None:
            return None
        try:
            return "boards/" + img.relative_to(_AURORA_BOARDS).as_posix()
        except ValueError:
            return None

_gbc.guess_image = _guess_image_safe

# gen_boards_catalog.get_catalog only loads Zephyr's vendor-prefixes.txt into its
# VndLookup, so Aurora's custom vendor prefix (auxspaceev) is unknown and the board
# catalog's vendor display falls back to the raw prefix string.  Patch get_catalog
# to inject the aurora vendor into the returned catalog's vendors dict.
#
# gen_boards_catalog also classifies any binding outside ZEPHYR_BASE/dts/bindings as
# "misc" (Miscellaneous).  Aurora's pyro bindings live in aurora/dts/bindings/pyro/
# and should be shown under a "pyro" category instead.  The patch below re-categorizes
# them after the catalog is built, and we also register "pyro" in the Zephyr domain's
# BINDING_TYPE_TO_DOCUTILS_NODE dict so the section heading renders correctly.
_AURORA_VENDOR_PREFIXES = _WORKSPACE / "aurora" / "dts" / "bindings" / "vendor-prefixes.txt"
_AURORA_PYRO_BINDINGS  = _WORKSPACE / "aurora" / "dts" / "bindings" / "pyro"
_orig_get_catalog = _gbc.get_catalog

def _get_catalog_with_aurora_fixes(**kwargs):
    catalog = _orig_get_catalog(**kwargs)

    # 1. Inject Aurora's vendor prefix so it resolves to "Auxspace e.V." in the catalog.
    try:
        import devicetree.edtlib as _edtlib
        aurora_vendors = _edtlib.load_vendor_prefixes_txt(_AURORA_VENDOR_PREFIXES)
        catalog["vendors"].update(aurora_vendors)
    except Exception as e:
        _logger.warning(
            "Failed to load Aurora vendor prefixes from %s: %s",
            _AURORA_VENDOR_PREFIXES,
            e,
        )

    # 2. Re-categorize features whose binding lives in aurora/dts/bindings/pyro/
    #    from the catch-all "misc" bucket to "pyro".
    # 3. Fix empty "locations" for DTS nodes whose source files live outside
    #    ZEPHYR_BASE (i.e. in Aurora's own board/SoC trees).  gen_boards_catalog
    #    only classifies files under ZEPHYR_BASE, so every Aurora-specific node
    #    ends up with an empty locations set and the on-chip/on-board column in
    #    the board-supported-hw table is left blank.
    # 4. Normalize aurora node filenames from absolute paths to "aurora: boards/..."
    #    so that zephyr.link-roles' zephyr_file role can produce correct GitHub
    #    links.  gen_boards_catalog leaves them as absolute paths because they are
    #    outside ZEPHYR_BASE; the role expects either a ZEPHYR_BASE-relative path
    #    (default "zephyr" module) or a "<module>: <path>" form.
    _AURORA_REPO = _AURORA_BOARDS.parent
    for board_data in catalog.get("boards", {}).values():
        for target_data in board_data.get("supported_features", {}).values():
            # As of Zephyr's board-catalog refactor, each board target maps to a
            # dict of {"ram_size", "flash_size", "features"}; the binding-type ->
            # compat buckets we care about live under the "features" sub-dict.
            features = target_data.get("features", {})
            misc = features.get("misc", {})
            to_move = {
                compat: fdata
                for compat, fdata in misc.items()
                if any(
                    Path(n["binding_path"]).is_relative_to(_AURORA_PYRO_BINDINGS)
                    for n in fdata.get("okay_nodes", []) + fdata.get("disabled_nodes", [])
                )
            }
            if to_move:
                features.setdefault("pyro", {}).update(to_move)
                for compat in to_move:
                    del misc[compat]

            for fdata in (
                fd
                for bucket in features.values()
                for fd in bucket.values()
            ):
                all_nodes = fdata.get("okay_nodes", []) + fdata.get("disabled_nodes", [])
                location_determined = bool(fdata.get("locations"))
                for node_info in all_nodes:
                    dts_path = Path(node_info["dts_path"])
                    if not dts_path.is_relative_to(_AURORA_REPO):
                        continue
                    # Determine location from the first aurora node found.
                    if not location_determined:
                        fdata["locations"].add(
                            "board" if dts_path.is_relative_to(_AURORA_BOARDS) else "soc"
                        )
                        location_determined = True
                    # Rewrite to "aurora: <repo-relative path>" for link generation.
                    node_info["filename"] = (
                        f"aurora: {dts_path.relative_to(_AURORA_REPO).as_posix()}"
                    )

    return catalog

_gbc.get_catalog = _get_catalog_with_aurora_fixes

# Register "pyro" in the Zephyr domain's binding-type heading dict.
# BINDING_TYPE_TO_DOCUTILS_NODE is populated at import time from Zephyr's
# binding-types.txt, which does not include Aurora's custom types.  Without
# this entry the table would fall back to rendering the raw key string "pyro".
import zephyr.domain as _zd
from docutils import nodes as _nodes
_zd.BINDING_TYPE_TO_DOCUTILS_NODE["pyro"] = _nodes.Text("Pyrotechnic Ignition")

# gh_utils.gh_link_get_url() builds the URL by joining
# base/mode/version/prefix/doc-path with "/".  Aurora's board pages resolve to
# an *empty* gh_link prefix (see gh_link_prefixes), so the join leaves a "//" in
# the path (e.g. .../blob/<ver>//boards/...), which breaks the board GitHub
# links (the "Browse board sources" button etc.).  Wrap the function -- the name
# the domain module actually calls -- to collapse duplicate slashes in the path
# while preserving the scheme's "//".
_orig_gh_link_get_url = _zd.gh_link_get_url

def _gh_link_get_url_clean(app, pagename, mode="blob"):
    url = _orig_gh_link_get_url(app, pagename, mode)
    if not url:
        return url
    scheme, sep, rest = url.partition("://")
    return scheme + sep + re.sub(r"/{2,}", "/", rest)

_zd.gh_link_get_url = _gh_link_get_url_clean

# Board status registry: board_id -> (display_name, status_string)
# Extend this dict when new boards are added or maintenance status changes.
_AURORA_BOARD_STATUS = {
    "sensor_board_v2": ("Auxspace Sensor Board V2", "Maintained"),
    "micrometer":      ("µMETER", "Maintained"),
}

# Patch ConvertBoardNode to ensure the Board Overview sidebar always has a
# "Status" field.  ConvertBoardNode.apply() builds the sidebar's field_list
# from the board YAML; if the YAML already contains a status entry we patch
# its value, otherwise we insert a new field.
_orig_convert_board_node_apply = _zd.ConvertBoardNode.apply

def _convert_board_node_with_status(self):
    _orig_convert_board_node_apply(self)

    try:
        docname = self.document.settings.env.docname
    except AttributeError:
        docname = ""

    # Rewrite the "Browse board sources" button (added by ConvertBoardNode) to a
    # clean directory URL.  Upstream builds href="{gh_link}/../..", which points
    # a blob URL at the board's doc file and then walks up two levels with
    # "/../.." -- a path only the browser normalises, and only after GitHub
    # redirects blob->tree.  Point it straight at the board directory instead.
    env = self.document.settings.env
    try:
        src_rel = os.path.relpath(env.doc2path(docname), env.srcdir).replace(os.sep, "/")
    except Exception:
        src_rel = ""
    board_dir = "/".join(src_rel.split("/")[:-2])  # strip the "/doc/<page>" tail
    if board_dir:
        clean_url = f"{gh_link_base_url}/tree/{gh_link_version}/{board_dir}"
        for raw in self.document.traverse(_nodes.raw):
            if "board-github-link" not in raw.astext():
                continue
            fixed = re.sub(r'href="[^"]*"', f'href="{clean_url}"', raw.astext(), count=1)
            raw.replace_self(_nodes.raw("", fixed, format="html"))
            break

    status_text = next(
        (status for board_id, (_, status) in _AURORA_BOARD_STATUS.items()
         if board_id in docname),
        "Not actively maintained",
    )

    for sidebar in self.document.traverse(_nodes.sidebar):
        if "board-overview" not in sidebar.get("classes", []):
            continue
        for field_list in sidebar.traverse(_nodes.field_list):
            # Patch an existing Status field if present.
            for field in field_list.children:
                if not isinstance(field, _nodes.field):
                    continue
                name_nodes = [c for c in field.children if isinstance(c, _nodes.field_name)]
                if name_nodes and name_nodes[0].astext() == "Status":
                    body_nodes = [c for c in field.children if isinstance(c, _nodes.field_body)]
                    if body_nodes:
                        body_nodes[0].clear()
                        body_nodes[0] += _nodes.paragraph(text=status_text)
                    return
            # Status field absent – insert one.
            field = _nodes.field()
            field += _nodes.field_name(text="Status")
            field_body = _nodes.field_body()
            field_body += _nodes.paragraph(text=status_text)
            field += field_body
            field_list += field
            return

_zd.ConvertBoardNode.apply = _convert_board_node_with_status


def _make_boards_status_table():
    """Build a docutils table listing every Aurora board and its status."""
    table = _nodes.table()
    tgroup = _nodes.tgroup(cols=2)
    table += tgroup
    tgroup += _nodes.colspec(colwidth=60)
    tgroup += _nodes.colspec(colwidth=40)

    thead = _nodes.thead()
    tgroup += thead
    hrow = _nodes.row()
    thead += hrow
    for header in ("Board", "Status"):
        entry = _nodes.entry()
        entry += _nodes.paragraph(text=header)
        hrow += entry

    tbody = _nodes.tbody()
    tgroup += tbody
    for _board_id, (name, status) in _AURORA_BOARD_STATUS.items():
        row = _nodes.row()
        tbody += row
        for cell in (name, status):
            entry = _nodes.entry()
            entry += _nodes.paragraph(text=cell)
            row += entry

    return table


def _on_doctree_resolved(app, doctree, docname):
    """Inject the board status table at the top of the boards index page."""
    if docname != "boards/index":
        return
    table = _make_boards_status_table()
    for section in doctree.traverse(_nodes.section):
        section.insert(1, table)
        break


def setup(app):
    app.connect("doctree-resolved", _on_doctree_resolved)
    app.connect("html-page-context", _add_aurora_github_url)

# -- Zephyr domain board features --------------------------------------------
# Run CMake-only twister pass for Aurora's boards so that board-supported-hw
# and board-supported-runners show real data instead of the "not generated" note.
# The vendor filter must match the 'vendor' field in the per-variant board YAML
# (e.g. sensor_board_v2_rp2040.yaml), which uses the vendor prefix "auxspaceev".
zephyr_generate_hw_features = True
zephyr_hw_features_vendor_filter = ["auxspaceev"]
# The twister pass runs with a minimal env that intentionally strips BOARD_ROOT,
# so Aurora's boards (which live outside ZEPHYR_BASE) would be invisible to it.
# Pass the board root on the command line instead.  _AURORA_BOARDS.parent is the
# aurora repo root, i.e. the directory *containing* boards/.
zephyr_hw_features_twister_extra_flags = ["--board-root", str(_AURORA_BOARDS.parent)]

# -- Options for zephyr.link-roles -------------------------------------------
# The zephyr_file role (used by board-supported-hw count indicators) needs to
# know that "aurora: boards/..." paths belong to the aurora repo.  Setting
# link_roles_manifest_project to "aurora" and link_roles_manifest_baseurl to
# the aurora GitHub URL makes the role resolve those paths correctly.
# The broken-links glob suppresses false-positive warnings for aurora board
# paths that don't exist under ZEPHYR_BASE (where the role would normally look).
link_roles_manifest_project = "aurora"
link_roles_manifest_baseurl = gh_link_base_url
link_roles_manifest_project_broken_links_ignore_globs = ["*"]
