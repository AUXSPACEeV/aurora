if (NOT DEFINED BOARD_SRC_PATH)
    message(FATAL_ERROR "Board '${PICO_BOARD}' not supported.")
endif ()

# Add the standard include files to the build
set(AURORA_INCLUDES
        ${FREERTOS_KERNEL_PATH}/Common/include
        ${BOARD_SRC_PATH}/include
        ${AURORA_SRC_PATH}/include
        )

add_subdirectory(drivers)
add_subdirectory(tasks)

# Add executable. Default name is the project name, version 0.1

add_executable(aurora main.c)
target_compile_features(aurora PRIVATE cxx_std_17)

target_include_directories(aurora PUBLIC
        ${AURORA_INCLUDES}
        )

# Add the standard library to the build
target_link_libraries(aurora PRIVATE
        FreeRTOS-Kernel
        FreeRTOS-Kernel-Heap4
        pico_stdlib
        pico_binary_info
        hardware_i2c
        hardware_spi
        hardware_watchdog
        )

target_link_libraries(aurora PUBLIC
        aurora_sdcard
        aurora_tasks
        )

pico_set_program_name(aurora "aurora")
pico_set_program_version(aurora "0.1")

pico_enable_stdio_uart(aurora 1)
pico_enable_stdio_usb(aurora 1)

pico_add_extra_outputs(aurora)
