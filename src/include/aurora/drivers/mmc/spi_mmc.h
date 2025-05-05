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
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/spi.h"

#include "pico/mutex.h"
#include "pico/sem.h"
#include "pico/types.h"

#include <aurora/drivers/mmc/mmc.h>

#ifndef BIT
#define BIT(x)  ((uint32_t)(1 << (x)))
#endif

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

/**
 * @brief SPI SDCard command structure (48 bits)
 * @note This structure is used to send commands to the SDCard.
 * @note The structure is packed to ensure that the data is aligned correctly.
 *
 * @param start: two start bits, set to "0b01"
 *
 * @param cmd: six bits for the command number
 *
 * @param arg: 32 bits for the arguments
 *
 * @param crc7: 7 bit CRC32 sum
 *
 * @param stop: at last one stop bit
 *
 * @ref https://users.ece.utexas.edu/~valvano/EE345M/SD_Physical_Layer_Spec.pdf
 */
struct spi_mmc_message {
    uint8_t start : 2;
    uint8_t cmd : 6;
    uint32_t arg;
    uint8_t crc7 : 7;
    uint8_t stop : 1;
}__attribute__((packed));

#define SPI_MMC_CMD_CRC(_cmd, _arg, _crc7) \
    ((struct spi_mmc_message){             \
        .start = 0b01,    /* 2 bits */     \
        .cmd   = (_cmd),  /* 6 bits */     \
        .arg   = (_arg),                   \
        .crc7  = (_crc7), /* 7 bits */     \
        .stop  = 1        /* 1 bit */      \
    })
#define SPI_MMC_CMD(_cmd, _arg)  SPI_MMC_CMD_CRC(_cmd, _arg, 0b1111111)

struct spi_config {
    spi_inst_t *hw_spi;
    uint miso_gpio;  // SPI MISO GPIO number (not pin number)
    uint mosi_gpio;
    uint sck_gpio;
    uint baud_rate;
    uint DMA_IRQ_num; // DMA_IRQ_0 or DMA_IRQ_1
    bool use_dma;

    // Drive strength levels for GPIO outputs.
    // enum gpio_drive_strength { GPIO_DRIVE_STRENGTH_2MA = 0, GPIO_DRIVE_STRENGTH_4MA = 1, GPIO_DRIVE_STRENGTH_8MA = 2,
    // GPIO_DRIVE_STRENGTH_12MA = 3 }
    bool set_drive_strength;
    enum gpio_drive_strength mosi_gpio_drive_strength;
    enum gpio_drive_strength sck_gpio_drive_strength;

    // State variables:
    uint tx_dma;
    uint rx_dma;
    dma_channel_config tx_dma_cfg;
    dma_channel_config rx_dma_cfg;
    irq_handler_t dma_isr; // Ignored: no longer used
    bool initialized;
    semaphore_t sem;
    mutex_t mutex;
};

/**
 * @brief SPI SDCard driver structure
 * @note This structure is used to hold the driver data for the SDCard.
 *
 * @param use_dma: true if DMA should be used, false otherwise
 *
 * @param spi: pointer to the SPI instance
 *
 * @param cs_pin: chip select pin number
 */
struct spi_mmc_context {
    struct spi_config *spi;
    uint cs_pin;
};

/**
 * @brief Initialize SPI SDCard driver structure
 *
 * @param spi: pointer to the SPI instance
 *
 * @param cs_pin: chip select pin number
 *
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

void set_spi_dma_irq_channel(bool useChannel1, bool shared);

/* [] END OF FILE */