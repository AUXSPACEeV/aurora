# Copyright (c) 2026 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(watchdog.c)
zephyr_library_sources_ifdef(CONFIG_AURORA_WDT_RECOVERY wdt_recovery.c)
