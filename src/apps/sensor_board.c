/**
 * @file sensor_board.c
 * @brief Aurora sensor board application
 *
 * Author: Maximilian Stephan @ Auxspace e.V.
 * Copyright (C) 2025 Auxspace e.V.
 */

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "pico/malloc.h"

#include <aurora/app.h>
#include <aurora/drivers/mmc/spi_mmc.h>

static struct spi_config *spi;
static struct mmc_drv *mmc;

/*----------------------------------------------------------------------------*/

static int setup_spi(void)
{
    spi = (struct spi_config *)malloc(sizeof(struct spi_config));
    if (spi == NULL) {
        printf("Could not allocate SPI driver.\n");
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

static void unsetup_spi(void)
{
    free(spi);
    spi = NULL;
}

/*----------------------------------------------------------------------------*/

static int setup_sdcard(void)
{
    int ret;

    if (spi == NULL) {
        printf("SPI driver not initialized.\n");
        return -EINVAL;
    }

    mmc = spi_mmc_drv_init(spi, PICO_DEFAULT_SPI_CSN_PIN);
    printf("PICO_DEFAULT_SPI_CSN_PIN %d\n", PICO_DEFAULT_SPI_CSN_PIN);
    if (mmc == NULL) {
        printf("SPI SD init failed.\n");
        return -ENODEV;
    }
    ret = mmc->ops->probe(mmc->dev);
    if (ret) {
        printf("SPI SD init failed: %d\n", ret);
        return ret;
    }

    printf("SPI initialised, let's goooooo\n");
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

    ret = setup_spi();
    if (ret) {
        printf("SPI init failed: %d\n", ret);
        return ret;
    }

    ret = setup_sdcard();
    if (ret) {
        printf("SD Card init failed: %d\n", ret);
        return ret;
    }

    return 0;
}

/*----------------------------------------------------------------------------*/

void aurora_hwdeinit(void)
{
    unsetup_spi();
}

/*----------------------------------------------------------------------------*/

void aurora_main(void)
{
    return;
}
/* EOF */