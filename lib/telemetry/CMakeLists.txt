# Copyright (c) 2026 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

zephyr_library()

zephyr_linker_sources(SECTIONS telemetry.ld)
zephyr_library_sources(telemetry.c)

if(CONFIG_AURORA_TELEMETRY_HC12)
  zephyr_library_sources(hc12/hc12.c)
endif()
