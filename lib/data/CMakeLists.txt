# Copyright (c) 2026 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

zephyr_library()

zephyr_library_sources(data_logger.c)

if(CONFIG_DATA_LOGGER_BIN)
    zephyr_library_sources(convert.c)
    if(CONFIG_DATA_LOGGER_BIN_BACKEND_FLASH)
        zephyr_library_sources(fmt_bin.c)
    elseif(CONFIG_DATA_LOGGER_BIN_BACKEND_DISK)
        zephyr_library_sources(fmt_bin_disk.c)
        if(CONFIG_DATA_LOGGER_DISK_AUTO_MKFS)
            zephyr_library_sources(flight_log_disk_auto_format.c)
            zephyr_library_link_libraries(ELMFAT)
        endif()
    endif()
endif()

if(CONFIG_DATA_LOGGER_CONVERT_CSV)
    zephyr_library_sources(fmt_csv.c)
endif()

if(CONFIG_DATA_LOGGER_CONVERT_INFLUX)
    zephyr_library_sources(fmt_influx.c)
endif()

if(CONFIG_DATA_LOGGER_SHELL)
    zephyr_library_sources(data_shell.c)
endif()
