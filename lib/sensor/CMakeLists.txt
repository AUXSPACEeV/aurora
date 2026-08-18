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

if(CONFIG_AURORA_FAKE_SENSORS)
    # Launch clock, flight profile and `sim` shell, shared by imu.c and baro.c.
    zephyr_library_sources(sim.c)
endif()

if(CONFIG_AURORA_FAKE_SENSORS_REPLAY)
    # Strip surrounding quotes Kconfig keeps on string symbols.
    string(REGEX REPLACE "^\"(.*)\"$" "\\1"
        REPLAY_INPUT_REL "${CONFIG_AURORA_FAKE_SENSORS_REPLAY_INPUT}")
    # Paths are relative to the aurora module root (the directory holding
    # zephyr/module.yml), two levels up from lib/sensor.
    set(REPLAY_INPUT "${CMAKE_CURRENT_SOURCE_DIR}/../../${REPLAY_INPUT_REL}")
    set(REPLAY_GEN  "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/gen_flight_replay.py")
    set(REPLAY_OUT  "${CMAKE_CURRENT_BINARY_DIR}/replay_data.c")

    if(NOT EXISTS "${REPLAY_INPUT}")
        message(FATAL_ERROR
            "AURORA_FAKE_SENSORS_REPLAY: input CSV not found: "
            "${REPLAY_INPUT}")
    endif()

    # If a state_audit sits next to flights.csv (default data_logger
    # layout), pass it through so the generator trims to the
    # [BOOST - 4 s, LANDED + 4 s] window.
    get_filename_component(REPLAY_INPUT_DIR "${REPLAY_INPUT}" DIRECTORY)
    set(REPLAY_AUDIT "${REPLAY_INPUT_DIR}/state_audit")
    set(REPLAY_AUDIT_ARGS)
    set(REPLAY_AUDIT_DEPS)
    if(EXISTS "${REPLAY_AUDIT}")
        list(APPEND REPLAY_AUDIT_ARGS --state-audit "${REPLAY_AUDIT}")
        list(APPEND REPLAY_AUDIT_DEPS "${REPLAY_AUDIT}")
        message("-- Found replay audit in flight dir: ${REPLAY_AUDIT}")
    endif()

    add_custom_command(
        OUTPUT  "${REPLAY_OUT}"
        COMMAND ${PYTHON_EXECUTABLE} "${REPLAY_GEN}"
            --input "${REPLAY_INPUT}"
            --output "${REPLAY_OUT}"
            ${REPLAY_AUDIT_ARGS}
        DEPENDS "${REPLAY_INPUT}" "${REPLAY_GEN}" ${REPLAY_AUDIT_DEPS}
        COMMENT "Generating replay_data.c from ${REPLAY_INPUT_REL}"
        VERBATIM
    )

    zephyr_library_sources("${REPLAY_OUT}")
    # replay_data.c is generated into the binary dir but includes replay.h
    # from here.
    zephyr_library_include_directories(${CMAKE_CURRENT_SOURCE_DIR})
endif()
