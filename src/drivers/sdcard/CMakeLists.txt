# Aurora SDCard Drivers

add_library(aurora_sdcard
        spi_sdcard.c
        )

target_link_libraries(aurora_sdcard
        pico_stdlib
        hardware_spi
        )

target_include_directories(aurora_sdcard PRIVATE
        ${AURORA_SRC_PATH}/include
        ${BOARD_SRC_PATH}/include
        )
