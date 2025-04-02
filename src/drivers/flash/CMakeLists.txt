# Aurora SDCard Drivers

add_library(aurora_flash
        spi_flash.c
        )

target_link_libraries(aurora_flash
        pico_stdlib
        pico_binary_info
        hardware_spi
        )

target_include_directories(aurora_flash PRIVATE
        ${AURORA_SRC_PATH}/include
        ${BOARD_SRC_PATH}/include
        )
