#-------------------------------------------------------------------------------
# Sensor Board prototype
#
# Copyright (c) 2025 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr)
project(sensor_board)

target_sources(app PRIVATE 
    src/main.c
    )

# Conditionally include storage.c if CONFIG_STORAGE is enabled
if(CONFIG_STORAGE)
    target_sources(app PRIVATE src/storage.c)
endif()
