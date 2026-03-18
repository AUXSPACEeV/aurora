# Copyright (c) 2025-2026 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

zephyr_library()

if(CONFIG_SIMPLE_STATE)
    zephyr_library_sources(simple_state.c)
endif()
