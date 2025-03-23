# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(PICO_BOARD pico CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

# Pull in FreeRTOS (must be before project)
include(FreeRTOS_Kernel_import.cmake)

if (PICO_SDK_VERSION_STRING VERSION_LESS "1.4.0")
    message(FATAL_ERROR "Raspberry Pi Pico SDK version 1.4.0 (or later) required. Your version is ${PICO_SDK_VERSION_STRING}")
endif()

project(aurora C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(aurora main.c )

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
        ${CMAKE_CURRENT_LIST_DIR}/.. # for our common lwipopts or any other standard includes, if required
        ${FREERTOS_KERNEL_PATH}/Common/include
        )

target_link_libraries(aurora
        hardware_watchdog
        )

pico_add_extra_outputs(aurora)

