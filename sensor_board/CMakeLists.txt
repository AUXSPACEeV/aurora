#-------------------------------------------------------------------------------
# Sensor Board prototype
#
# Copyright (c) 2025-2026 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# TODO: Exclude zephyr from SCA since it produces false positives
# set(ZEPHYR_SCA_VARIANT gcc)

# Make application-local snippets (e.g. ./snippets/sim) discoverable by
# `west build -S <name>` and twister's SNIPPET= extra_args.
list(APPEND SNIPPET_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

# The "app" library target below is created by find_package(Zephyr) (see
# zephyr/cmake/modules/kernel.cmake), independently of the project name.
project(sensor_board LANGUAGES C CXX)

target_sources(app PRIVATE
	src/main.c
	src/data.c
	)
