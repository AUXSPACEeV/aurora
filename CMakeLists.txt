# Works with 3.14
cmake_minimum_required(VERSION 3.14)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake" ${CMAKE_MODULE_PATH})

if (DEFINED ENV{AURORA_SRC_PATH} AND (NOT AURORA_SRC_PATH))
    set(AURORA_SRC_PATH $ENV{AURORA_SRC_PATH})
    message("Using AURORA_SRC_PATH from environment ('${AURORA_SRC_PATH}')")
endif ()

# Pull in Raspberry Pi Pico SDK (must be before project)
include(${CMAKE_MODULE_PATH}/pico_sdk_import.cmake)

if (PICO_SDK_VERSION_STRING VERSION_LESS "1.4.0")
    message(FATAL_ERROR "Raspberry Pi Pico SDK version 1.4.0 (or later) required. Your version is ${PICO_SDK_VERSION_STRING}")
endif()

if (NOT DEFINED AURORA_SRC_PATH)
    set(AURORA_SRC_PATH ${CMAKE_CURRENT_LIST_DIR}/src)
    message("Using default AURORA_SRC_PATH ('${AURORA_SRC_PATH}')")
endif ()

if (DEFINED ENV{PICO_BOARD} AND (NOT PICO_BOARD))
    set(PICO_BOARD $ENV{PICO_BOARD})
    message("Using PICO_BOARD from environment ('${PICO_BOARD}')")
else ()
    set(PICO_BOARD "pico")
    message("Using default PICO_BOARD ('${PICO_BOARD}')")
endif ()

# Pull in FreeRTOS (must be before project)
if (PICO_BOARD STREQUAL "pico2_w" OR PICO_BOARD STREQUAL "pico2")
    set( BOARD_SRC_PATH ${AURORA_SRC_PATH}/board/rp2350_arm CACHE STRING "Board path")
endif()

if (PICO_BOARD STREQUAL "pico_w" OR PICO_BOARD STREQUAL "pico")
    set( BOARD_SRC_PATH ${AURORA_SRC_PATH}/board/rp2040 CACHE STRING "Board path")
endif ()

include(${BOARD_SRC_PATH}/FreeRTOS_Kernel_import.cmake)

# Project name and a few useful settings. Other commands can pick up the results
project(
  auxspace_aurora
  VERSION 0.1
  DESCRIPTION "AUxspace ROcket opeRAting System"
  LANGUAGES C CXX ASM
  )

# Initialise the Raspberry Pi Pico SDK before diving into subdirectories
pico_sdk_init()
  
# The compiled library code is here
add_subdirectory(src)
