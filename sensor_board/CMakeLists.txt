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

# For some reason, when using west twister, the app target is not created
# automatically, so we need to create it manually here.
# Building locally and without the zephyr github action, naming this
# "sensor_board" works fine, but as soon as we use west twister inside a
# github-runner context, the target must be named "app":
# > Cannot specify sources for target "app" which is not built by this project.
project(app LANGUAGES C)

target_sources(app PRIVATE
	src/main.c
	)

target_sources_ifdef(CONFIG_AURORA_FAKE_SENSORS app PRIVATE
	src/fake_sensors.c
	)
