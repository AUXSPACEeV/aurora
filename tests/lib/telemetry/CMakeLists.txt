# Copyright (c) 2026 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aurora_lib_telemetry_test)

FILE(GLOB app_sources src/*.c)
target_sources(app PRIVATE ${app_sources})

# Tests need to call hc12_frame_finalise directly to lock the wire
# format. The header is private to the backend, so add it to the
# include path explicitly.
target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../lib/telemetry/hc12)
