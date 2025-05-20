/**
 * @file sensor_board.c
 * @brief Aurora sensor board application
 *
 * Author: Maximilian Stephan @ Auxspace e.V.
 * Copyright (C) 2025 Auxspace e.V.
 */

#include "FreeRTOS.h"
#include "task.h"

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "pico/malloc.h"

#include <aurora/app.h>
#include <aurora/drivers/spi.h>
#include <aurora/drivers/mmc/spi_mmc.h>
#include <aurora/log.h>

static struct spi_config *spi;
static struct mmc_drv *mmc;

/*----------------------------------------------------------------------------*/

static int setup_spi_config(void)
{
    spi = (struct spi_config *)malloc(sizeof(struct spi_config));
    if (spi == NULL) {
        log_error("Could not allocate SPI driver.\n");
        return -ENOMEM;
    }

    spi->hw_spi = spi_default;
    spi->miso_gpio = PICO_DEFAULT_SPI_RX_PIN;  // GPIO number (not pin number)
    spi->mosi_gpio = PICO_DEFAULT_SPI_TX_PIN;
    spi->sck_gpio = PICO_DEFAULT_SPI_SCK_PIN;
    spi->baud_rate = 12500 * 1000;
    spi->use_dma = false;  // do not use DMA for now

    return 0;
}

/*----------------------------------------------------------------------------*/

static void unsetup_spi_config(void)
{
    free(spi);
    spi = NULL;
}

/*----------------------------------------------------------------------------*/

static int setup_sdcard(void)
{
    int ret;

    if (spi == NULL) {
        log_error("SPI driver not initialized.\n");
        return -EINVAL;
    }

    mmc = spi_mmc_drv_init(spi, PICO_DEFAULT_SPI_CSN_PIN);
    /* TODO: replace CS pin with configurable options and test them */
    log_debug("PICO_DEFAULT_SPI_CSN_PIN %d\n", PICO_DEFAULT_SPI_CSN_PIN);
    if (mmc == NULL) {
        log_error("SPI SD init failed.\n");
        return -ENODEV;
    }
    ret = mmc->ops->probe(mmc->dev);
    if (ret) {
        log_error("SPI SD init failed: %d\n", ret);
        return ret;
    }

    log_debug("Sensor board hardware initialised!\n");
}

/*----------------------------------------------------------------------------*/

static void unsetup_sdcard(void)
{
    spi_mmc_drv_deinit(mmc);
}

/*----------------------------------------------------------------------------*/

int aurora_hwinit(void)
{
    int ret;

    ret = setup_spi_config();
    if (ret) {
        log_error("SPI init failed: %d\n", ret);
        return ret;
    }

    ret = setup_sdcard();
    if (ret) {
        log_error("SD Card init failed: %d\n", ret);
        return ret;
    }

    return 0;
}

/*----------------------------------------------------------------------------*/

void aurora_hwdeinit(void)
{
    unsetup_spi_config();
}

/*----------------------------------------------------------------------------*/

void aurora_main(void)
{
    const TickType_t xDelay = 3000 / portTICK_PERIOD_MS;

    for(;;) {
        vTaskDelay(xDelay);
        if (mmc->ops->n_sectors)
            log_info("Sectors on card: %llu\n", mmc->ops->n_sectors(mmc->dev));
        if (mmc->ops->blk_read) {
            uint8_t *data = calloc(1, mmc->dev->blksize);
            int ret = mmc->ops->blk_read(mmc->dev, 0x0, data, 1);
            if (ret) {
                log_error("ERROR reading blocks: %d\n", ret);
            }
            hexdump(data, mmc->dev->blksize);
            free(data);
        }
    }
}
/* EOF */