# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

if (DEFINED ENV{PICO_BOARD} AND (NOT PICO_BOARD))
    set(PICO_BOARD $ENV{PICO_BOARD})
    message("Using PICO_BOARD from environment ('${PICO_BOARD}')")
else ()
    set(PICO_BOARD "pico")
    message("Using default PICO_BOARD ('${PICO_BOARD}')")
endif ()

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

# Pull in FreeRTOS (must be before project)
if (PICO_BOARD STREQUAL "pico2_w" OR PICO_BOARD STREQUAL "pico2")
    set( BOARD_SRC_PATH ${CMAKE_CURRENT_LIST_DIR}/board/rp2350_arm CACHE STRING "Board path")
endif()

if (PICO_BOARD STREQUAL "pico_w" OR PICO_BOARD STREQUAL "pico")
    set( BOARD_SRC_PATH ${CMAKE_CURRENT_LIST_DIR}/board/rp2040 CACHE STRING "Board path")
endif ()

if (NOT DEFINED BOARD_SRC_PATH)
    message(FATAL_ERROR "Board '${PICO_BOARD}' not supported.")
endif ()

include(${BOARD_SRC_PATH}/FreeRTOS_Kernel_import.cmake)

if (PICO_SDK_VERSION_STRING VERSION_LESS "1.4.0")
    message(FATAL_ERROR "Raspberry Pi Pico SDK version 1.4.0 (or later) required. Your version is ${PICO_SDK_VERSION_STRING}")
endif()

project(aurora C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(aurora main.c)

pico_set_program_name(aurora "aurora")
pico_set_program_version(aurora "0.1")

pico_enable_stdio_uart(aurora 1)
pico_enable_stdio_usb(aurora 1)

# Add the standard library to the build
target_link_libraries(aurora
        FreeRTOS-Kernel
        FreeRTOS-Kernel-Heap4
        pico_stdlib)

# Add the standard include files to the build
target_include_directories(aurora PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        ${FREERTOS_KERNEL_PATH}/Common/include
        ${BOARD_SRC_PATH}/include
        )

target_link_libraries(aurora
        hardware_i2c
        hardware_watchdog
        )

pico_add_extra_outputs(aurora)

