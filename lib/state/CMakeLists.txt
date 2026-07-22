# Copyright (c) 2025-2026 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

zephyr_library()

# Common state machine core (implementation-independent).
zephyr_library_sources(state.c)

if(CONFIG_SIMPLE_STATE)
    zephyr_library_sources(simple.c)
endif()

if(CONFIG_AURORA_STATE_MACHINE_AUDIT)
    zephyr_library_sources(state_audit.c)
endif()

if(CONFIG_AURORA_STATE_MACHINE_SHELL)
    zephyr_library_sources(state_shell.c)
endif()
