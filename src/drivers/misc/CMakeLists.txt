# Aurora MISC Drivers

add_library(aurora_misc
        crc.c
        )

target_link_libraries(aurora_misc
        pico_stdlib
        )

target_include_directories(aurora_misc PRIVATE
        ${AURORA_SRC_PATH}/include
        ${BOARD_SRC_PATH}/include
        )
