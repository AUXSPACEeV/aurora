# Aurora Common Lib

add_library(aurora_common
        log.c
        )

target_link_libraries(aurora_common
        pico_stdlib
        pico_binary_info
        pico_malloc
        pico_mem_ops
        )

target_include_directories(aurora_common PRIVATE
        ${AURORA_SRC_PATH}/include
        ${BOARD_SRC_PATH}/include
        )
