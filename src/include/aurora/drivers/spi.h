/**
 * @file spi.h
 * @brief SPI driver header
 *
 * Author: Maximilian Stephan @ Auxspace e.V.
 * Copyright (C) 2025 Auxspace e.V.
 */
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include "pico/stdlib.h"
#include "pico/mutex.h"
#include "pico/sem.h"
#include "pico/types.h"

#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/spi.h"

#include <aurora/list.h>
#include <aurora/macros.h>

#define SPI_TRANSFER_FLG_NONE           BIT(0)
#define SPI_TRANSFER_FLG_XFER_START     BIT(1)
#define SPI_TRANSFER_FLG_XFER_STOP      BIT(2)

/**
 * @brief SPI SDCard driver configuration state structure
 *
 * @param tx_dma: DMA channel for TX
 * @param rx_dma: DMA channel for RX
 * @param tx_dma_cfg: DMA channel configuration for TX
 * @param rx_dma_cfg: DMA channel configuration for RX
 * @param initialized: true if the state is initialized, false otherwise
 * @param sem: semaphore for synchronization
 *
 * @note This structure is used to hold the state of the SPI SDCard driver and
 * is set by the driver.
 */
struct spi_config_state {
    // State variables:
    uint tx_dma;
    uint rx_dma;
    dma_channel_config tx_dma_cfg;
    dma_channel_config rx_dma_cfg;
    bool initialized;
    semaphore_t sem;
};

/**
 * @brief SPI SDCard driver configuration structure
 *
 * @param hw_spi: pointer to the SPI instance from the pico-sdk
 * @param miso_gpio: SPI MISO GPIO number (not pin number)
 * @param mosi_gpio: SPI MOSI GPIO number (not pin number)
 * @param sck_gpio: SPI SCK GPIO number (not pin number)
 * @param baud_rate: SPI baud rate in Hz
 * @param DMA_IRQ_num: DMA IRQ number (DMA_IRQ_0 or DMA_IRQ_1)
 * @param use_dma: true if DMA should be used, false otherwise
 * @param set_drive_strength: true if drive strength should be set, false otherwise
 * @param mosi_gpio_drive_strength: drive strength for MOSI GPIO
 * @param sck_gpio_drive_strength: drive strength for SCK GPIO
 * @param state: Pointer to the SPI configuration state. Set by the driver.
 * @param node: list node for the SPI configuration
 *
 * @note This structure is used to configure the SPI SDCard driver.
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
    struct list_head node;
};

/*----------------------------------------------------------------------------*/

/**
 * @brief get total number of registered SPI drivers
 *
 * @return number of registered SPI drivers
 */
size_t spi_get_num();

/*----------------------------------------------------------------------------*/

/**
 * @brief get spi driver instance by number
 * 
 * @param num: number of spi instance
 * @return: pointer to spi config instance
 */
struct spi_config *spi_get_by_num(size_t num);

/*----------------------------------------------------------------------------*/

/**
 * @brief Set the SPI DMA IRQ channel
 *
 * @param useChannel1: true if channel 1 should be used, false otherwise
 * @param shared: true if the IRQ should be shared, false otherwise
 */
void set_spi_dma_irq_channel(bool useChannel1, bool shared);

/*----------------------------------------------------------------------------*/

/**
 * @brief perform an SPI transfer using the configured DMA channels
 * 
 * @param spi: pointer to SPI config
 * @param tx: SPI transfer TX buffer
 * @param rx: SPI transfer RX buffer
 * @param length: Total length of the SPI transfer
 * @return: Error code
 */
int spi_transfer_dma(struct spi_config *spi, const uint8_t *tx, uint8_t* rx,
                     size_t length);

/*----------------------------------------------------------------------------*/

/**
 * @brief SPI pinmuxing and bus init
 * 
 * @param spi: spi_config pointer to driver configuration
 * @return error code
 */
int aurora_spi_init(struct spi_config *spi);

/*----------------------------------------------------------------------------*/

/**
 * @brief SPI deinit
 * 
 * @param spi: spi_config pointer to driver configuration
 */
void aurora_spi_deinit(struct spi_config *spi);

/*----------------------------------------------------------------------------*/

/**
 * @brief Assert chip select
 * 
 * @param cs_pin: Pin number of chip select
 */
static inline void cs_select(uint cs_pin)
{
    asm volatile("nop \n nop \n nop"); // FIXME
    gpio_put(cs_pin, 0);
    asm volatile("nop \n nop \n nop"); // FIXME
}

/*----------------------------------------------------------------------------*/

/**
 * @brief Deassert chip select
 * 
 * @param cs_pin: Pin number of chip select
 */
static inline void cs_deselect(uint cs_pin)
{
    asm volatile("nop \n nop \n nop"); // FIXME
    gpio_put(cs_pin, 1);
    asm volatile("nop \n nop \n nop"); // FIXME
}

/*----------------------------------------------------------------------------*/

/**
 * @brief Lock the SPI state semaphore
 * 
 * @param spi: spi config to lock
 */
static inline bool spi_lock(struct spi_config *spi)
{
    return sem_acquire_timeout_ms(&spi->state->sem, 1);
}

/*----------------------------------------------------------------------------*/

/**
 * @brief Unlock the SPI state mutex
 * 
 * @param spi: spi config to unlock
 */
static inline void spi_unlock(struct spi_config *spi)
{
    sem_release(&spi->state->sem);
}

/* [] END OF FILE */