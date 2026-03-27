# Copyright (c) 2025-2026 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

"""
Compatibility shim for using zephyr.domain outside a full Zephyr doc build.

zephyr.domain depends on config values normally registered by other Zephyr
extensions (zephyr.gh_utils) and replaces Breathe's ``doxygengroup`` directive
with a Zephyr-specific one that drops the ``:content-only:`` option.

This extension (loaded after zephyr.domain) fixes both issues.
"""

from breathe.directives.content_block import DoxygenGroupDirective as BreatheDoxygenGroupDirective

_GH_LINK_KEYS = ("gh_link_base_url", "gh_link_version", "gh_link_prefixes", "gh_link_exclude")
_LINK_ROLES_KEYS = (
    "link_roles_manifest_project",
    "link_roles_manifest_baseurl",
    "link_roles_manifest_project_broken_links_ignore_globs",
)


def _apply_gh_link_config(app):
    """Copy gh_link_* and link_roles_manifest_* values from the raw conf.py
    namespace into the live config.

    Sphinx calls Config.init_values() before extension setup() runs, so any
    config key registered inside setup() never receives its conf.py value —
    the extension-provided default wins instead.  This builder-inited handler
    runs after all setup() calls and patches the values from the raw namespace.
    """
    raw = getattr(app.config, "_raw_config", {})
    for key in _GH_LINK_KEYS + _LINK_ROLES_KEYS:
        if key in raw:
            setattr(app.config, key, raw[key])


def setup(app):
    # Register config values that zephyr.domain reads from zephyr.gh_utils, and
    # config values that zephyr.link-roles registers in its own setup().
    # Without this, transforms crash with "No such config value: ...".
    for name, default in [
        ("gh_link_base_url", ""),
        ("gh_link_version", ""),
        ("gh_link_prefixes", {}),
        ("gh_link_exclude", []),
        ("link_roles_manifest_project", None),
        ("link_roles_manifest_baseurl", None),
        ("link_roles_manifest_project_broken_links_ignore_globs", []),
    ]:
        try:
            app.add_config_value(name, default, "")
        except Exception:
            pass  # already registered

    # Apply conf.py values once the app is fully initialised (see docstring above).
    app.connect("builder-inited", _apply_gh_link_config)

    # Restore Breathe's doxygengroup directive, which zephyr.domain replaces
    # with a Zephyr-specific version that does not support :content-only:.
    app.add_directive("doxygengroup", BreatheDoxygenGroupDirective, override=True)

    return {"version": "0.1", "parallel_read_safe": True, "parallel_write_safe": True}
