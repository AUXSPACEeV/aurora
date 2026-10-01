# Copyright (c) 2026 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

zephyr_library()

zephyr_linker_sources(SECTIONS telemetry.ld)
zephyr_library_sources(telemetry.c)

# Transport engine shared by the UART-based backends below.
if(CONFIG_AURORA_TELEMETRY_UART_LINK_ENGINE)
  zephyr_library_sources(uart_link/uart_link.c)
endif()

if(CONFIG_AURORA_TELEMETRY_UART_LINK)
  zephyr_library_sources(uart_link/uart_link_backend.c)
endif()

if(CONFIG_AURORA_TELEMETRY_HC12)
  zephyr_library_sources(hc12/hc12.c)
endif()

if(CONFIG_AURORA_TELEMETRY_HC12_AT)
  zephyr_library_sources(hc12/at.c)
endif()

if(CONFIG_AURORA_TELEMETRY_HC12_SHELL)
  zephyr_library_sources(hc12/hc12_shell.c)
endif()
