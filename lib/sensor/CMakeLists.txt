# Copyright (c) 2025-2026 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

zephyr_library()

if(CONFIG_IMU)
    zephyr_library_sources(imu.c)
endif()

if(CONFIG_ATTITUDE)
    zephyr_library_sources(attitude.c)
endif()

if(CONFIG_BARO)
    zephyr_library_sources(baro.c)
endif()