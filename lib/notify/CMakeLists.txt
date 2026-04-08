# Copyright (c) 2026 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

zephyr_library()

zephyr_linker_sources(SECTIONS notify.ld)
zephyr_library_sources(notify.c)

if(CONFIG_AURORA_NOTIFY_BUZZER)
  zephyr_library_sources(buzzer/buzzer.c)
endif()
