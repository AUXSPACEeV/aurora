# Aurora SDCard Drivers

# TODO: Choose which config to use
add_library(aurora_apps
        sensor_board.c
        )

target_link_libraries(aurora_apps
        FreeRTOS-Kernel
        pico_stdlib
        pico_binary_info
        pico_malloc
        pico_mem_ops
        hardware_dma
        hardware_irq
        hardware_gpio
        hardware_spi
        aurora_mmc
        aurora_misc
        )

target_include_directories(aurora_apps PRIVATE
        ${AURORA_SRC_PATH}/include
        ${FREERTOS_KERNEL_PATH}/Common/include
        ${BOARD_SRC_PATH}/include
        )
