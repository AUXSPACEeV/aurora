# Copyright (c) 2025-2026 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

"""
Sphinx extension providing a ``zephyr`` domain with board documentation
directives compatible with the Zephyr project conventions.

Supported directives
--------------------

``.. zephyr:board:: <board-name>``
    Marks a page as the documentation for a board. Must appear before any
    ``zephyr:board-supported-hw`` or ``zephyr:board-supported-runners``
    directives on the same page.

``.. zephyr:board-supported-hw::``
    Renders a note describing where hardware feature support is configured
    (devicetree).  Must be placed inside a ``zephyr:board`` page.

``.. zephyr:board-supported-runners::``
    Renders a table of the west runners available for the board.  Must be
    placed inside a ``zephyr:board`` page.
"""

import os
import yaml

from docutils import nodes
from sphinx.domains import Domain
from sphinx.util import logging
from sphinx.util.docutils import SphinxDirective

logger = logging.getLogger(__name__)

# Runners known to be relevant for boards in this project.
# Extend this list as new targets are added.
_BOARD_RUNNERS = {
    "rp2040": [
        ("uf2",        True,  False),
        ("openocd",    False, True),
        ("jlink",      False, False),
        ("pyocd",      False, False),
        ("blackmagicprobe", False, False),
    ],
    "rp2350a": [
        ("uf2",        True,  False),
        ("openocd",    False, True),
        ("jlink",      False, False),
        ("pyocd",      False, False),
    ],
    "esp32s3": [
        ("esp_idf",    True,  True),
        ("openocd",    False, False),
        ("jlink",      False, False),
    ],
}

_DEFAULT_RUNNERS = _BOARD_RUNNERS["rp2040"]


def _board_yml_path(docname: str, app) -> str | None:
    """Try to locate the board.yml for the board whose doc page is *docname*."""
    # docname is e.g. "boards/auxspace/sensor_board_v2/doc/sensor_board_v2"
    # Walk up to find a board.yml sibling.
    parts = docname.split("/")
    for i in range(len(parts), 0, -1):
        candidate = os.path.join(app.srcdir, *parts[:i], "board.yml")
        if os.path.isfile(candidate):
            return candidate
        # Also look one level up (doc/ subdirectory pattern)
        candidate = os.path.join(app.srcdir, *parts[:i], "..", "board.yml")
        if os.path.isfile(candidate):
            return os.path.normpath(candidate)
    return None


def _load_socs(docname: str, app) -> list[str]:
    """Return the list of SoC names from board.yml, if found."""
    path = _board_yml_path(docname, app)
    if not path:
        return []
    try:
        with open(path) as f:
            data = yaml.safe_load(f)
        socs = data.get("board", {}).get("socs", [])
        return [s["name"] for s in socs if "name" in s]
    except Exception:
        return []


# ---------------------------------------------------------------------------
# Directives
# ---------------------------------------------------------------------------

class BoardDirective(SphinxDirective):
    """``.. zephyr:board:: <board-name>``"""

    required_arguments = 1
    optional_arguments = 0
    has_content = False

    def run(self):
        board_name = self.arguments[0]
        env = self.env

        if not hasattr(env, "aurora_board_pages"):
            env.aurora_board_pages = {}
        if not hasattr(env, "aurora_current_board"):
            env.aurora_current_board = {}

        existing = env.aurora_board_pages.get(board_name)
        if existing and existing != env.docname:
            logger.warning(
                f"Board '{board_name}' is already documented in '{existing}'.",
                location=(env.docname, self.lineno),
            )
        else:
            env.aurora_board_pages[board_name] = env.docname

        env.aurora_current_board[env.docname] = board_name
        return []


class BoardSupportedHardwareDirective(SphinxDirective):
    """``.. zephyr:board-supported-hw::``"""

    required_arguments = 0
    optional_arguments = 0
    has_content = False

    def run(self):
        env = self.env
        board_name = getattr(env, "aurora_current_board", {}).get(env.docname)

        if not board_name:
            logger.warning(
                "zephyr:board-supported-hw must be used inside a zephyr:board page.",
                location=(env.docname, self.lineno),
            )
            return []

        socs = _load_socs(env.docname, env.app)
        soc_text = f" ({', '.join(socs)})" if socs else ""

        note = nodes.note()
        para = nodes.paragraph()
        para += nodes.Text(
            f"The supported hardware features for the "
        )
        para += nodes.literal(text=board_name)
        para += nodes.Text(
            f" board{soc_text} are determined by its devicetree configuration. "
            "Refer to the board's ``.dts`` and ``.dtsi`` files for the full list "
            "of enabled peripherals and their pin assignments."
        )
        note += para
        return [note]


class BoardSupportedRunnersDirective(SphinxDirective):
    """``.. zephyr:board-supported-runners::``"""

    required_arguments = 0
    optional_arguments = 0
    has_content = False

    def run(self):
        env = self.env
        board_name = getattr(env, "aurora_current_board", {}).get(env.docname)

        if not board_name:
            logger.warning(
                "zephyr:board-supported-runners must be used inside a zephyr:board page.",
                location=(env.docname, self.lineno),
            )
            return []

        socs = _load_socs(env.docname, env.app)
        # Pick runner table based on the first recognised SoC, fall back to RP2040.
        runners = _DEFAULT_RUNNERS
        for soc in socs:
            if soc in _BOARD_RUNNERS:
                runners = _BOARD_RUNNERS[soc]
                break

        # Build a simple RST-style table as docutils nodes.
        table = nodes.table()
        table["classes"].append("colwidths-auto")

        tgroup = nodes.tgroup(cols=3)
        table += tgroup
        for _ in range(3):
            tgroup += nodes.colspec()

        # Header
        thead = nodes.thead()
        tgroup += thead
        hrow = nodes.row()
        thead += hrow
        for label in ("Runner", "Flash", "Debug"):
            entry = nodes.entry()
            entry += nodes.paragraph(text=label)
            hrow += entry

        # Body
        tbody = nodes.tbody()
        tgroup += tbody
        for runner, is_flash_default, is_debug_default in runners:
            row = nodes.row()
            tbody += row

            name_entry = nodes.entry()
            name_entry += nodes.literal(text=runner)
            row += name_entry

            flash_entry = nodes.entry()
            flash_entry += nodes.paragraph(
                text="\u2713 (default)" if is_flash_default else "\u2713"
                if not is_debug_default else ""
            )
            row += flash_entry

            debug_entry = nodes.entry()
            debug_entry += nodes.paragraph(
                text="\u2713 (default)" if is_debug_default else ""
            )
            row += debug_entry

        note = nodes.note()
        note_para = nodes.paragraph()
        note_para += nodes.Text(
            "Run "
        )
        note_para += nodes.literal(text="west flash --help")
        note_para += nodes.Text(
            " or "
        )
        note_para += nodes.literal(text="west debug --help")
        note_para += nodes.Text(
            " to see all available options for the selected runner."
        )
        note += note_para

        return [table, note]


# ---------------------------------------------------------------------------
# Domain
# ---------------------------------------------------------------------------

class ZephyrDomain(Domain):
    """Minimal ``zephyr`` Sphinx domain providing board documentation directives."""

    name = "zephyr"
    label = "Zephyr"

    directives = {
        "board": BoardDirective,
        "board-supported-hw": BoardSupportedHardwareDirective,
        "board-supported-runners": BoardSupportedRunnersDirective,
    }


# ---------------------------------------------------------------------------
# Extension entry point
# ---------------------------------------------------------------------------

def setup(app):
    app.add_domain(ZephyrDomain)
    return {
        "version": "0.1",
        "parallel_read_safe": True,
        "parallel_write_safe": True,
    }
