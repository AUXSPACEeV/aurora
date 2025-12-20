#-------------------------------------------------------------------------------
# Sensor Board prototype
#
# Copyright (c) 2025 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# TODO: Exclude zephyr from SCA since it produces false positives
# set(ZEPHYR_SCA_VARIANT gcc)

find_package(Zephyr)
project(sensor_board)

# Build cdc-acm-console with snippet
if(CONFIG_USB_SERIAL)
	if(NOT cdc-acm-console IN_LIST SNIPPET)
		set(SNIPPET cdc-acm-console ${SNIPPET} CACHE STRING "" FORCE)
	endif()
endif()

target_sources(app PRIVATE
	src/main.c
	)

# Conditionally include storage.c if CONFIG_STORAGE is enabled
if(CONFIG_STORAGE)
	target_sources(app PRIVATE src/storage.c)
endif()
