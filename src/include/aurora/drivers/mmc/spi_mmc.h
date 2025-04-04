/**
* @file spi_mmc.h
* @brief SPI micro SD Card library
* @note This file contains the function prototypes and definitions for
* SPI micro SD (uSD) Cards.
*
* Author: Maximilian Stephan @ Auxspace e.V.
* Copyright (C) 2025 Auxspace e.V.
*/

#ifndef _SPI_MMC_H
#define _SPI_MMC_H

#include <stdio.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"

#include <aurora/drivers/mmc/mmc.h>

typedef struct spi_mmc_dev_data {
    bool use_dma;
    spi_inst_t *spi;
    uint cs_pin;
} spi_mmc_dev_data_t;

mmc_drv_t *spi_mmc_drv_init(spi_inst_t *spi, uint cs_pin, bool use_dma);

void spi_mmc_drv_deinit(mmc_drv_t *drv);

#endif /* _SPI_MMC_H */
