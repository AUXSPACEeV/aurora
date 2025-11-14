/**
* @file spi_mmc.h
* @brief SPI micro SD Card library
* @note This file contains the function prototypes and definitions for
* SPI micro SD (uSD) Cards.
*
* Author: Maximilian Stephan @ Auxspace e.V.
* Copyright (C) 2025 Auxspace e.V.
*/

#pragma once

#include <stdio.h>
#include <stdbool.h>

#include "pico/stdlib.h"
#include "pico/mutex.h"
#include "pico/sem.h"
#include "pico/types.h"

#include <aurora/list.h>
#include <aurora/drivers/mmc/mmc.h>
#include <aurora/drivers/spi.h>

/* MMC/SD in SPI mode reports R1 status always */
#define R1_SPI_IDLE			    BIT(0)
#define R1_SPI_ERASE_RESET		BIT(1)
#define R1_SPI_ILLEGAL_COMMAND	BIT(2)
#define R1_SPI_COM_CRC			BIT(3)
#define R1_SPI_ERASE_SEQ		BIT(4)
#define R1_SPI_ADDRESS			BIT(5)
#define R1_SPI_PARAMETER		BIT(6)
/* R1 bit 7 is always zero, reuse this bit for error */
#define R1_SPI_ERROR			BIT(7)

#define SPI_MMC_START_BLOCK     (0xFE)

/**
 * @brief SPI SDCard command macro
 *
 * @param _cmd: command number
 * @param _arg: command argument
 * @param _crc7: CRC7 checksum
 */
#define SPI_MMC_CMD_CRC(_cmd, _arg, _crc7) \
    ((struct spi_mmc_message){             \
        .start = 0b01,    /* 2 bits */     \
        .cmd   = (_cmd),  /* 6 bits */     \
        .arg   = (_arg),                   \
        .crc7  = (_crc7), /* 7 bits */     \
        .stop  = 1        /* 1 bit */      \
    })

/**
 * @brief SPI SDCard command macro without CRC7 checksum
 *
 * @param _cmd: command number
 * @param _arg: command argument
 *
 * @note This macro sets the CRC7 checksum to 0b1111111.
 */
#define SPI_MMC_CMD(_cmd, _arg)  SPI_MMC_CMD_CRC(_cmd, _arg, 0b1111111)

/**
 * @brief SPI SDCard driver specific context
 *
 * @param spi: pointer to the SPI configuration data structure
 * @param cs_pin: chip select pin number
 *
 * @note This structure is used to hold the driver context for the SDCard.
 */
struct spi_mmc_context {
    struct spi_config *spi;
    uint cs_pin;
};

/**
 * @brief Initialize SPI SDCard driver structure
 *
 * @param spi: pointer to the SPI instance
 * @param cs_pin: chip select pin number
 * @param use_dma: true if DMA should be used, false otherwise
 *
 * @return pointer to the newly created SPI SDCard driver structure
 */
struct mmc_drv *spi_mmc_drv_init(struct spi_config *spi, uint cs_pin);

/**
 * @brief Uninitialize the SPI SDCard driver structure
 *
 * @param drv: pointer to the SPI SDCard driver structure to uninitialize
 */
void spi_mmc_drv_deinit(struct mmc_drv *drv);

/* [] END OF FILE */