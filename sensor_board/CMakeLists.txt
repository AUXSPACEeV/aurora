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

target_sources_ifdef(CONFIG_AURORA_FAKE_SENSORS_SYNTH app PRIVATE
	src/fake_sensors.c
	)

if(CONFIG_AURORA_FAKE_SENSORS_REPLAY)
	# Strip surrounding quotes Kconfig keeps on string symbols.
	string(REGEX REPLACE "^\"(.*)\"$" "\\1"
		REPLAY_INPUT_REL "${CONFIG_AURORA_FAKE_SENSORS_REPLAY_INPUT}")
	set(REPLAY_INPUT "${CMAKE_CURRENT_SOURCE_DIR}/../${REPLAY_INPUT_REL}")
	set(REPLAY_GEN  "${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_flight_replay.py")
	set(REPLAY_OUT  "${CMAKE_CURRENT_BINARY_DIR}/replay_data.c")

	if(NOT EXISTS "${REPLAY_INPUT}")
		message(FATAL_ERROR
			"AURORA_FAKE_SENSORS_REPLAY: input CSV not found: "
			"${REPLAY_INPUT}")
	endif()

	add_custom_command(
		OUTPUT  "${REPLAY_OUT}"
		COMMAND ${PYTHON_EXECUTABLE} "${REPLAY_GEN}"
			--input "${REPLAY_INPUT}"
			--output "${REPLAY_OUT}"
		DEPENDS "${REPLAY_INPUT}" "${REPLAY_GEN}"
		COMMENT "Generating replay_data.c from ${REPLAY_INPUT_REL}"
		VERBATIM
	)

	target_sources(app PRIVATE
		src/fake_sensors_replay.c
		"${REPLAY_OUT}"
	)
	target_include_directories(app PRIVATE src)
endif()
