# Copyright (c) 2026 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

zephyr_library()

zephyr_library_sources(data_logger.c)

if(CONFIG_DATA_LOGGER_CSV)
    zephyr_library_sources(fmt_csv.c)
endif()

if(CONFIG_DATA_LOGGER_INFLUX)
    zephyr_library_sources(fmt_influx.c)
endif()

if(CONFIG_DATA_LOGGER_SHELL)
    zephyr_library_sources(data_shell.c)
endif()
