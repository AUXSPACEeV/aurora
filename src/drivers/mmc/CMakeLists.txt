# Aurora SDCard Drivers

add_library(aurora_mmc
        spi_mmc.c
        )

target_link_libraries(aurora_mmc
        pico_stdlib
        pico_binary_info
        pico_malloc
        pico_mem_ops
        hardware_spi
        )

target_include_directories(aurora_mmc PRIVATE
        ${AURORA_SRC_PATH}/include
        ${BOARD_SRC_PATH}/include
        )
