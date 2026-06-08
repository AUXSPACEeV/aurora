# Copyright (c) 2026 Auxspace e.V.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aurora_lib_pad_link_test)

FILE(GLOB app_sources src/*.c)
target_sources(app PRIVATE ${app_sources})

# pad_link_wire.h is private to the library; expose it to the test so
# the wire-format suite can check struct layout against the same
# definition the production code uses.
target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../lib/pad_link)
