# Copyright (c) 2026 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

add_subdirectory(auxspaceev)

zephyr_library_sources_ifdef(CONFIG_PYRO_SHELL pyro_shell.c)
