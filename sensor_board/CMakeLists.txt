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

# Conditionally include usb_serial.c if CONFIG_USB_SERIAL is enabled
if(CONFIG_USB_SERIAL)
    target_sources(app PRIVATE src/usb_serial.c)
endif()

# Conditionally include usb_serial.c if CONFIG_SIMPLE_STORAGE is enabled
if(CONFIG_SIMPLE_STORAGE)
    target_sources(app PRIVATE src/simple_storage.c)
endif()

# Conditionally include imu.c if CONFIG_IMU is enabled
if(CONFIG_IMU)
    target_sources(app PRIVATE src/imu.c)
endif()