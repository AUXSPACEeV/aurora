/**
 * @file spi_mmc.c
 * @brief Sources for MMC/SD I/O via SPI
 * @note This file contains the source code for MMC/SD I/O via SPI.
 *
 * Author: Maximilian Stephan @ Auxspace e.V.
 * Copyright (C) 2025 Auxspace e.V.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/binary_info.h"
#include "pico/malloc.h"
#include "pico/mem_ops.h"
#include "hardware/spi.h"
#include "errno.h"

#include <aurora/drivers/mmc/mmc.h>
#include <aurora/drivers/mmc/spi_mmc.h>

/**
 * @brief SPI SDCard command structure (48 bits)
 * @note This structure is used to send commands to the SDCard.
 * @note The structure is packed to ensure that the data is aligned correctly.
 * https://users.ece.utexas.edu/~valvano/EE345M/SD_Physical_Layer_Spec.pdf
 */
struct spi_mmc_message {
    /* two start bits, set to "0b01" */
    uint8_t start : 2;
    /* six bits for the command number */
    uint8_t cmd : 6;
    /* 32 Bits for the arguments */
    uint32_t arg;
    /**
     * OPTIONAL: 7 bit CRC32 sum
     * the sdcard only checks for a correct crc32 sum if it is configured
     * to do so and when evaluating CMD8
     */
    uint8_t crc32_le : 7;
    /* at last one stop bit */
    uint8_t stop : 1;
}__attribute__((packed));

static inline void cs_select(uint cs_pin)
{
    asm volatile("nop \n nop \n nop"); // FIXME
    gpio_put(cs_pin, 0);
    asm volatile("nop \n nop \n nop"); // FIXME
}

static inline void cs_deselect(uint cs_pin)
{
    asm volatile("nop \n nop \n nop"); // FIXME
    gpio_put(cs_pin, 1);
    asm volatile("nop \n nop \n nop"); // FIXME
}

static int send_msg(spi_inst_t *spi,
    uint cs_pin, const struct spi_mmc_message *msg, uint8_t* response)
{
    int ret;
    cs_select(cs_pin);
    if (response)
        ret = spi_write_read_blocking(spi, (uint8_t *)msg, sizeof(*msg), response);
    else
        ret = spi_write_blocking(spi, (uint8_t *)msg, sizeof(*msg));
    cs_deselect(cs_pin);
    if (ret != sizeof(*msg)) {
        printf("Error: %d\n", ret);
        return -EIO;
    }
    return 0;
}

static int send_reset(spi_inst_t *spi, uint cs_pin)
{
    const struct spi_mmc_message reset_cmd = {
        .start = 0b01,
        .cmd = MMC_CMD_GO_IDLE_STATE,
        .arg = 0,
        .crc32_le = 0b1001010,
        .stop = 1,
    };

    return send_msg(spi, cs_pin, &reset_cmd, NULL);
}

static int check_response(uint8_t *resp, mmc_response_t resp_type) {
    if (resp == NULL) {
        return -EINVAL;
    }

    switch(resp_type) {
        case MMC_RESP_R1:
            uint8_t resp_val = *resp;
            return (resp_val & (R1_SPI_COM_CRC | R1_SPI_ERASE_SEQ |
                                R1_SPI_ADDRESS | R1_SPI_ILLEGAL_COMMAND))
        default:
            printf("No such response type: %s\n", resp_type);
            return -EINVAL;
    }

    return 0;
}

static int send_voltage_verify(spi_inst_t *spi, uint cs_pin)
{
    uint8_t response = 0;
    int ret;
    const struct spi_mmc_message cmd8 = {
        .start = 0b01,
        .cmd = MMC_CMD_SEND_EXT_CSD,
        .arg = 0x40000000,
        .crc32_le = 0b1001010,  // crc is ignored for this command
        .stop = 1,
    };
    const struct spi_mmc_message acmd41 = {
        .start = 0b01,
        .cmd = SD_CMD_APP_SEND_OP_COND,
        .arg = 0,
        .crc32_le = 0,  // crc is ignored for this command
        .stop = 1,
    };

    send_msg(spi, cs_pin, &cmd8, NULL);
    ret = send_msg(spi, cs_pin, &reset_cmd, &response);
    if (ret) {
        printf("Sending ACMD41 failed.\n");
    }
    return check_response(&response, MMC_RESP_R1);
}

int spi_mmc_probe(mmc_dev_t *dev)
{
    if (dev->initialized == true) {
        return 0;
    }

    struct spi_mmc_message spi_init_message = { 0 };

    int ret;
    uint i;
    spi_mmc_dev_data_t *data = (spi_mmc_dev_data_t *)dev->priv;
    if (!data) {
        printf("No SPI MMC data available!\n");
        return -EINVAL;
    }

    // Wait for at least 74 cycles with MOSI and CS asserted
    memset(&spi_init_message, 1, sizeof(spi_init_message))
    for (i = 0; i < (74 / (sizeof(spi_init_message) * 8) + 1); i++) {
        send_msg(data->spi, data->cs_pin, &spi_init_message, NULL)
    }

    /**
     * First, put the SDCard into SPI mode by sendin CMD0, followed by CMD8.
     * CMD8 is optional, but it is recommended to send it to check if the card
     * is compatible with the SD spec.
     * CMD8 is only supported by SDHC and SDXC cards and not by MMC cards.
     * 
     * After CMD8, we send ACMD41.
     * ACMD41 is a synchronization command used to negotiate the operation
     * voltage range and to poll the cards until they are out of their power-up
     * sequence.
     * In case the host system connects multiple cards, the host shall check
     * that all cards satisfy the supplied voltage.
     * Otherwise, the host should select one of the cards and initialize.
     */
    send_reset(data->spi, data->cs_pin);
    ret = send_voltage_verify(data->spi, data->cs_pin);
    if (ret != 0) {
        printf("SPI init CMD8 failed.\n");
        goto error;
    }
    dev->initialized = true;

error:
    dev->initialized = false;
    return ret;
}

void spi_mmc_dbg_printbuf(mmc_dev_t *dev, uint8_t *buf)
{
    uint32_t i;
    for (i = 0; i < dev->num_blocks; ++i) {
        if (i % 16 == 15)
            printf("%02x\n", buf[i]);
        else
            printf("%02x ", buf[i]);
    }
}

mmc_drv_t *spi_mmc_drv_init(spi_inst_t *spi, uint cs_pin, bool use_dma)
{
    spi_mmc_dev_data_t *data = (spi_mmc_dev_data_t *)
        calloc(1, sizeof(spi_mmc_dev_data_t));
    if (!data) {
        printf("Could not allocate SPI MMC data: %d\n", -ENOMEM);
        return NULL;
    }
    data->use_dma = use_dma;
    data->spi = spi;
    data->cs_pin = cs_pin;

    mmc_dev_t *dev = (mmc_dev_t *)calloc(1, sizeof(mmc_dev_t));
    if (!dev) {
        printf("Could not allocate SPI MMC device: %d\n", -ENOMEM);
        goto free_data;
    }
    dev->name = "spi_mmc";
    dev->priv = data;

    mmc_ops_t *ops = (mmc_ops_t *)malloc(sizeof(mmc_ops_t));
    if (!ops) {
        printf("Could not allocate SPI MMC ops: %d\n", -ENOMEM);
        goto free_dev;
    }
    ops->probe = &spi_mmc_probe;
    ops->blk_read = NULL; // TODO
    ops->blk_write = NULL; // TODO
    ops->blk_erase = NULL; // TODO

    mmc_drv_t *drv = calloc(1, sizeof(mmc_drv_t));
    if (!drv) {
        printf("Could not allocate SPI MMC driver: %d\n", -ENOMEM);
        goto free_ops;
    }
    drv->dev = dev;
    drv->ops = ops;

    return drv;

free_ops:
    free(ops);
free_dev:
    free(dev);
free_data:
    free(data);
    return NULL;
}

void spi_mmc_drv_deinit(mmc_drv_t *drv)
{
    if (!drv)
        return;

    if (drv->dev) {
        free(drv->dev->priv);
        free(drv->dev);
    }
    if (drv->ops)
        free(drv->ops);
    free(drv);
}
