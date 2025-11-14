# Aurora SDCard Drivers

set(AURORA_MMC_SOURCES
    mmc_core.c
)

set(AURORA_MMC_LIBS
        pico_stdlib
        pico_binary_info
        pico_malloc
        pico_mem_ops
        hardware_gpio
        aurora_drivers
)

if(CONFIG_SPI_MMC)
    list(APPEND AURORA_MMC_SOURCES
        spi_mmc.c
        )

    list(APPEND AURORA_MMC_LIBS
        hardware_dma
        hardware_irq
        hardware_spi
        )
endif()

add_library(aurora_mmc
        ${AURORA_MMC_SOURCES}
        )

target_link_libraries(aurora_mmc
        ${AURORA_MMC_LIBS}
        )

target_include_directories(aurora_mmc PRIVATE
        ${AURORA_INCLUDES}
        )
