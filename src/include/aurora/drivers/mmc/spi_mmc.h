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

#ifndef BIT
#define BIT(x)  ((uint32_t)1 << (x))
#endif

/* MMC/SD in SPI mode reports R1 status always */
#define R1_SPI_IDLE			BIT(0)
#define R1_SPI_ERASE_RESET		BIT(1)
#define R1_SPI_ILLEGAL_COMMAND		BIT(2)
#define R1_SPI_COM_CRC			BIT(3)
#define R1_SPI_ERASE_SEQ		BIT(4)
#define R1_SPI_ADDRESS			BIT(5)
#define R1_SPI_PARAMETER		BIT(6)
/* R1 bit 7 is always zero, reuse this bit for error */
#define R1_SPI_ERROR			BIT(7)

struct spi_mmc_dev_data {
    bool use_dma;
    spi_inst_t *spi;
    uint cs_pin;
};

struct mmc_drv *spi_mmc_drv_init(spi_inst_t *spi, uint cs_pin, bool use_dma);

void spi_mmc_drv_deinit(struct mmc_drv *drv);

#endif /* _SPI_MMC_H */
