# Aurora Application Sources

set (AURORA_APP_LIBS
        FreeRTOS-Kernel
        pico_stdlib
        pico_binary_info
        pico_malloc
        pico_mem_ops
)

if (CONFIG_SENSOR_BOARD)
        set(AURORA_APP_SOURCES
                sensor_board.c
                )

        list(APPEND AURORA_APP_LIBS
                hardware_dma
                hardware_irq
                hardware_gpio
                hardware_spi
                aurora_mmc
                aurora_misc
                )
else ()
        set (AURORA_APP_SOURCES
                none_app.c
                )
endif()

add_library(aurora_apps
        ${AURORA_APP_SOURCES}
        )

target_link_libraries(aurora_apps
        ${AURORA_APP_LIBS}
        )

target_include_directories(aurora_apps PRIVATE
        ${AURORA_INCLUDES}
        )
