# Aurora Drivers

set (AURORA_INCLUDES ${AURORA_INCLUDES})

set (AURORA_DRV_SOURCES
        spi.c
        )

set(AURORA_DRV_LIBS
        pico_stdlib
        pico_binary_info
        pico_malloc
        pico_mem_ops
        hardware_dma
        hardware_irq
        hardware_gpio
        hardware_spi
)

if(CONFIG_MMC)
    list(APPEND AURORA_DRV_LIBS
        aurora_mmc
        aurora_misc
        )
endif()

add_library(aurora_drivers
        ${AURORA_DRV_SOURCES}
        )

add_subdirectory(mmc)
add_subdirectory(misc)

target_link_libraries(aurora_drivers
        ${AURORA_DRV_LIBS}
        )

target_include_directories(aurora_drivers PRIVATE
        ${AURORA_INCLUDES}
        )
