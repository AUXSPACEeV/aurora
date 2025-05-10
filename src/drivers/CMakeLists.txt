# Aurora Drivers

add_library(aurora_drivers
        spi.c
        )

add_subdirectory(mmc)
add_subdirectory(misc)

target_link_libraries(aurora_drivers
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

target_include_directories(aurora_drivers PRIVATE
        ${AURORA_SRC_PATH}/include
        ${BOARD_SRC_PATH}/include
        )
