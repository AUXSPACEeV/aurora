# Copyright (c) 2026 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

"""
``zephyr:shield`` directive for AUX-Stack shield documentation pages.

Zephyr's ``zephyr.domain`` only provides ``zephyr:board``, which renders the
"Board Overview" sidebar (image, vendor, status, sources link) from the board
catalog.  Shields are part of that same catalog (``domaindata["zephyr"]
["shields"]``, filled from each ``shield.yml``) but have no directive, so their
pages had neither a vendor label nor the shield image.

This extension adds the missing directive.  Usage (MyST)::

    ```{zephyr:shield} pwr65
    ```

Like ``zephyr:board``, it must be the first thing on the page: every node that
follows it is moved into a new section titled with the shield's ``full_name``,
next to a "Shield Overview" sidebar.

Must be loaded after ``zephyr.domain``.
"""

import os

from docutils import nodes
from sphinx.transforms import SphinxTransform
from sphinx.util import logging
from sphinx.util.docutils import SphinxDirective

logger = logging.getLogger(__name__)


class ShieldNode(nodes.Element):
    pass


class ShieldDirective(SphinxDirective):
    has_content = False
    required_arguments = 1
    optional_arguments = 0

    def run(self):
        shield_name = self.arguments[0]

        shields = self.env.domaindata["zephyr"]["shields"]
        vendors = self.env.domaindata["zephyr"]["vendors"]

        if shield_name not in shields:
            logger.warning(
                f"Shield {shield_name} does not seem to be a valid shield name.",
                location=(self.env.docname, self.lineno),
            )
            return []

        shield = shields[shield_name]
        node = ShieldNode(id=shield_name)
        node["full_name"] = shield["full_name"]
        node["vendor"] = vendors.get(shield["vendor"], shield["vendor"])
        node["image"] = shield["image"]
        return [node]


class ConvertShieldNode(SphinxTransform):
    # Run before ConvertBoardNode (100): conf.py's patched ConvertBoardNode
    # post-processes every "board-overview" sidebar on the page, and must see
    # the shield's too so that both stay consistent.
    default_priority = 99

    def apply(self):
        for node in list(self.document.findall(ShieldNode)):
            self.convert_node(node)

    def _status(self):
        status = self.config.aurora_board_status
        return next(
            (s for board_id, (_, s) in status.items() if board_id in self.env.docname),
            "Not actively maintained",
        )

    def _sources_url(self):
        # The page lives at <shield dir>/doc/<page>; link to <shield dir>.
        src_rel = os.path.relpath(self.env.doc2path(self.env.docname), self.env.srcdir)
        shield_dir = "/".join(src_rel.replace(os.sep, "/").split("/")[:-2])
        return (
            f"{self.config.gh_link_base_url}/tree/{self.config.gh_link_version}/{shield_dir}"
        )

    def convert_node(self, node):
        parent = node.parent
        if parent is None:
            return
        index = parent.index(node)
        siblings_to_move = parent.children[index + 1 :]

        new_section = nodes.section(ids=[node["id"]])
        new_section += nodes.title(text=node["full_name"])

        # Same classes as the board sidebar so it shares its styling.
        sidebar = nodes.sidebar(classes=["board-overview"])
        new_section += sidebar
        sidebar += nodes.title(text="Shield Overview")

        if node["image"] is not None:
            figure = nodes.figure()
            # scale=100 makes Sphinx link the image to its full-size version.
            figure += nodes.image(uri=f"/{node['image']}", scale=100)
            figure += nodes.caption(text=node["full_name"])
            sidebar += figure

        field_list = nodes.field_list()
        sidebar += field_list
        details = [
            ("Name", nodes.literal(text=node["id"])),
            ("Vendor", node["vendor"]),
            ("Status", self._status()),
        ]
        for property_name, value in details:
            field = nodes.field()
            field += nodes.field_name(text=property_name)
            field_body = nodes.field_body()
            if isinstance(value, nodes.Node):
                value = nodes.paragraph("", "", value)
            else:
                value = nodes.paragraph(text=value)
            field_body += value
            field += field_body
            field_list += field

        sidebar += nodes.raw(
            "",
            f"""
            <div id="board-github-link">
                <a href="{self._sources_url()}" class="btn btn-info fa fa-github"
                    target="_blank">
                    Browse shield sources
                </a>
            </div>
            """,
            format="html",
        )

        new_section.extend(siblings_to_move)
        node.replace_self(new_section)
        for sibling in siblings_to_move:
            parent.remove(sibling)


def setup(app):
    # board_id -> (display_name, status); shared with the board pages in conf.py.
    app.add_config_value("aurora_board_status", {}, "env", types=[dict])

    app.add_directive_to_domain("zephyr", "shield", ShieldDirective)
    app.add_transform(ConvertShieldNode)

    return {"version": "0.1", "parallel_read_safe": True, "parallel_write_safe": True}
