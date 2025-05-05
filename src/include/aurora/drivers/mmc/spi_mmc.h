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

/**
 * @brief SPI SDCard driver configuration state structure
 * @note This structure is used to hold the state of the SPI SDCard driver and
 * is set by the driver.
 *
 * @param tx_dma: DMA channel for TX
 *
 * @param rx_dma: DMA channel for RX
 *
 * @param tx_dma_cfg: DMA channel configuration for TX
 *
 * @param rx_dma_cfg: DMA channel configuration for RX
 *
 * @param initialized: true if the state is initialized, false otherwise
 *
 * @param sem: semaphore for synchronization
 *
 * @param mutex: mutex for synchronization
 */
struct spi_config_state {
    // State variables:
    uint tx_dma;
    uint rx_dma;
    dma_channel_config tx_dma_cfg;
    dma_channel_config rx_dma_cfg;
    bool initialized;
    semaphore_t sem;
    mutex_t mutex;
};

/**
 * @brief SPI SDCard driver configuration structure
 * @note This structure is used to configure the SPI SDCard driver.
 *
 * @param hw_spi: pointer to the SPI instance from the pico-sdk
 *
 * @param miso_gpio: SPI MISO GPIO number (not pin number)
 *
 * @param mosi_gpio: SPI MOSI GPIO number (not pin number)
 *
 * @param sck_gpio: SPI SCK GPIO number (not pin number)
 *
 * @param baud_rate: SPI baud rate in Hz
 *
 * @param DMA_IRQ_num: DMA IRQ number (DMA_IRQ_0 or DMA_IRQ_1)
 *
 * @param use_dma: true if DMA should be used, false otherwise
 *
 * @param set_drive_strength: true if drive strength should be set, false otherwise
 *
 * @param mosi_gpio_drive_strength: drive strength for MOSI GPIO
 *
 * @param sck_gpio_drive_strength: drive strength for SCK GPIO
 *
 * @param state: Pointer to the SPI configuration state. Set by the driver.
 */
struct spi_config {
    spi_inst_t *hw_spi;
    uint miso_gpio;
    uint mosi_gpio;
    uint sck_gpio;
    uint baud_rate;
    uint DMA_IRQ_num;
    bool use_dma;
    bool set_drive_strength;
    enum gpio_drive_strength mosi_gpio_drive_strength;
    enum gpio_drive_strength sck_gpio_drive_strength;
    struct spi_config_state *state;
};

/**
 * @brief SPI SDCard driver specific context
 * @note This structure is used to hold the driver context for the SDCard.
 *
 * @param spi: pointer to the SPI configuration data structure
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

/**
 * @brief Set the SPI DMA IRQ channel
 *
 * @param useChannel1: true if channel 1 should be used, false otherwise
 *
 * @param shared: true if the IRQ should be shared, false otherwise
 */
void set_spi_dma_irq_channel(bool useChannel1, bool shared);

/* [] END OF FILE */