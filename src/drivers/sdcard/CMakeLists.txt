# Aurora SDCard Drivers

add_library(aurora_sdcard
        spi_sd.c
        )

target_link_libraries(aurora_sdcard
        pico_stdlib
        pico_binary_info
        pico_malloc
        pico_mem_ops
        hardware_spi
        )

target_include_directories(aurora_sdcard PRIVATE
        ${AURORA_SRC_PATH}/include
        ${BOARD_SRC_PATH}/include
        )
