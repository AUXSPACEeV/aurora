# Copyright (c) 2026 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

zephyr_library()

if(CONFIG_FILTER_KALMAN)
    zephyr_library_sources(kalman.c)
endif()
