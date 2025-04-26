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
#include <stdbool.h>
#include "pico/stdlib.h"
#include "pico/binary_info.h"
#include "pico/malloc.h"
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
    uint8_t crc7 : 7;
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

static size_t spi_mmc_resp_size_raw(mmc_response_t resp_type)
{
    switch (resp_type) {
        case MMC_RESP_R1:
        case MMC_RESP_R1b:
        case MMC_RESP_R3:
        case MMC_RESP_R6:
        case MMC_RESP_R7:
            return 6;  // 48 Bits
        case MMC_RESP_R2:
            return 17;  // 136 Bits
        default:
            return 0;
    }
}

static size_t spi_mmc_resp_size(mmc_response_t resp_type)
{
    switch (resp_type) {
        case MMC_RESP_R1:
        case MMC_RESP_R1b:
            return 1;   // 8 Bits
        case MMC_RESP_R2:
            return 2;  // 16 Bits
        case MMC_RESP_R3:
        case MMC_RESP_R6:
        case MMC_RESP_R7:
            return 5;   // 40 Bits
        default:
            return 0;
    }
}

static int spi_mmc_parse_response(uint8_t *dst, uint8_t *resp,
                                  mmc_response_t resp_type)
{
    /**
     * IMPORTANT: This function does not check the size of the resp buffer.
     * If you call this function, make sure that your buffer has the appropriate
     * size of the response by using spi_mmc_resp_size.
     * 
     * Also, dst carries sizeof(struct spi_mmc_message) garbage in the front.
     */
    uint8_t *raw_dst = dst + sizeof(struct spi_mmc_message);
    uint i;
    if (resp == NULL) {
        printf("spi_mmc_parse_response: resp is NULL.\n");
        return -EINVAL;
    }

    for (i = 0; i < spi_mmc_resp_size(resp_type); i++) {
        resp[i] = raw_dst[i];
    }
    return 0;
}

static int send_msg(struct spi_mmc_dev_data *data,
                    const struct spi_mmc_message *msg, uint8_t* resp,
                    mmc_response_t resp_type)
{
    /**
     * @note: no nullptr error handling done here for better performance.
     * Make sure values are valid before calling send_msg!
     */
    int ret;
    uint8_t *dst;
    uint8_t tmp;
    uint i;
    uint cs_pin = data->cs_pin;
    spi_inst_t *spi = data->spi;

    if (data->use_dma) {
        printf("DMA not supported yet\n");
        return -ENOTSUP;
    }

    cs_select(cs_pin);
    if (resp == NULL && resp_type == MMC_RESP_NONE) {
        ret = spi_write_blocking(spi, (uint8_t *)msg, sizeof(*msg));
    } else {
        dst = (uint8_t *)calloc(1, sizeof(uint8_t));
        if (!dst) {
            printf("Could not allocate response buffer: %d\n", -ENOMEM);
            return -ENOMEM;
        }

        // Write and read separately, since SDCard can take a while to respond
        ret = spi_write_blocking(spi, (uint8_t *)msg, sizeof(*msg));
        for (i = 0; i < 0XFF; i++) {
            ret |= spi_read_blocking(spi, 0xFF, dst, sizeof(uint8_t));
            if (*dst != 0xFF) {
                break;
            } else if (ret) {
                printf("Could not read from SD Card.\n");
                free(dst);
                return -EIO;
            }
        }
        // read the rest of the response
        if (spi_mmc_resp_size(resp_type) > 1) {
            tmp = *dst;
            free(dst);
            dst = (uint8_t *)calloc(1, spi_mmc_resp_size(resp_type));
            spi_read_blocking(spi, 0xFF, &dst[1],
                              spi_mmc_resp_size(resp_type) - 1);
            memcpy(dst, &tmp, 1);
        }
        if (resp && spi_mmc_parse_response(dst, resp, resp_type)) {
            printf("Error parsing SPI response: %d\n", ret);
            free(dst);
            return -EIO;
        }
        free(dst);
    }
    cs_deselect(cs_pin);
    if (ret != sizeof(*msg)) {
        printf("Error: %d\n", ret);
        return -EIO;
    }
    return 0;
}

static int send_reset(struct spi_mmc_dev_data *data)
{
    int ret = 0;
    uint retries = 0;
    const uint max_retries = 10;
    const struct spi_mmc_message reset_cmd = {
        .start = 0b01,
        .cmd = MMC_CMD_GO_IDLE_STATE,
        .arg = 0,
        .crc7 = 0x4A,
        .stop = 1,
    };
    uint8_t *resp = (uint8_t *)calloc(1, spi_mmc_resp_size(MMC_RESP_R1));
    while (!ret && !(*resp & R1_SPI_IDLE) && (retries++ <= max_retries)) {
        ret = send_msg(data, &reset_cmd, resp, MMC_RESP_R1);
    }
    if (!ret && retries > max_retries) {
        printf("MMC SPI reset command exceeded max num of retries.\n");
        ret = -ETIMEDOUT;
    }

    free(resp);
    return ret;
}

static bool check_response(uint8_t *resp, mmc_response_t resp_type)
{
    if (resp == NULL) {
        printf("No response available!\n");
        return false;
    }

    switch(resp_type) {
        case MMC_RESP_R1:
            return (*resp & (R1_SPI_COM_CRC | R1_SPI_ERASE_SEQ |
                                R1_SPI_ADDRESS | R1_SPI_ILLEGAL_COMMAND)) == 0;
        default:
            printf("No such response type: %s\n", resp_type);
            return false;
    }

    return true;
}

static int spi_mmc_voltage_select(struct mmc_dev *dev)
{
    const uint max_num_retries = 10;
    uint num_retries = 0;
    uint8_t *response;
    int ret;
    struct spi_mmc_dev_data *data = (struct spi_mmc_dev_data *)dev->priv;
    struct spi_mmc_message acmd41 = {
        .start = 0b01,
        .cmd = SD_CMD_APP_SEND_OP_COND,
        .arg = 0,
        .crc7 = 0x7f,  // crc is ignored for this command
        .stop = 1,
    };
    const struct spi_mmc_message cmd8 = {
        .start = 0b01,
        .cmd = MMC_CMD_SEND_EXT_CSD,
        .arg = 0x1AA,
        .crc7 = 0x43,
        .stop = 1,
    };
    const struct spi_mmc_message cmd55 = {
        .start = 0b01,
        .cmd = MMC_CMD_APP_CMD,
        .arg = 0,
        .crc7 = 0x7f,
        .stop = 1,
    };
    const struct spi_mmc_message cmd58 = {
        .start = 0b01,
        .cmd = MMC_CMD_SPI_READ_OCR,
        .arg = 0,
        .crc7 = 0x7f,
        .stop = 1,
    };

    response = (uint8_t *) calloc(1, spi_mmc_resp_size(MMC_RESP_R7));
    ret = send_msg(data, &cmd8, response, MMC_RESP_R7);
    if (response[0] & R1_SPI_ILLEGAL_COMMAND) {
        dev->version = SD_CARD_TYPE_SD1;
    // only need last byte of r7 response (echo-back)
    } else if(response[4] & 0xAA) {
        dev->version = SD_CARD_TYPE_SD2;
    } else {
        printf("Card did not respond to voltage select.\n");
        ret = -EIO;
        goto free_resp;
    }

    // set acmd41 arg depending on version
    acmd41.arg = dev->version == SD_CARD_TYPE_SD2 ? 0X40000000 : 0;

    // free response and alloc again with R1 size
    free(response);
    response = (uint8_t *)calloc(1, spi_mmc_resp_size(MMC_RESP_R1));
    *response = 1;

    do {
        ret = send_msg(data, &cmd55, NULL, MMC_RESP_R1);
        if (ret) {
            printf("Sending CMD55 failed.\n");
            ret = -EIO;
            goto free_resp;
        }

        ret = send_msg(data, &acmd41, response, MMC_RESP_R1);
        if (ret) {
            printf("Sending ACMD41 failed.\n");
            ret = -EIO;
            goto free_resp;
        } else if(num_retries++ >= max_num_retries) {
            printf("Sending ACMD41 timed out.\n");
            ret = -ETIMEDOUT;
            goto free_resp;
        }
    } while(*response != 0);
    free(response);

    // if SD2 read OCR register to check for SDHC card
    if (dev->version == SD_CARD_TYPE_SD2) {
        response = (uint8_t *)calloc(1, spi_mmc_resp_size(MMC_RESP_R3));
        if (send_msg(data, &cmd58, response, MMC_RESP_R3)) {
            printf("Sending CMD58 failed.\n");
            ret = -EIO;
            goto free_resp;
        }
        if ((response[1] & 0XC0) == 0XC0) {
            dev->version = SD_CARD_TYPE_SDHC;
        }
        // discard rest of ocr - contains allowed voltage range
    }

free_resp:
    free(response);
    return ret;
}

static int spi_mmc_init(struct mmc_dev *dev)
{
    struct spi_mmc_message spi_init_message = { 0 };

    int ret;
    uint i;
    struct spi_mmc_dev_data *data = (struct spi_mmc_dev_data *)dev->priv;
    if (!data) {
        printf("No SPI MMC data available!\n");
        return -EINVAL;
    }

    // Wait for at least 74 cycles with MOSI and CS asserted
    memset(&spi_init_message, 1, sizeof(spi_init_message));
    for (i = 0; i < (74 / (sizeof(spi_init_message) * 8) + 1); i++) {
        send_msg(data, &spi_init_message, NULL, MMC_RESP_NONE);
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
    send_reset(data);
    ret = spi_mmc_voltage_select(dev);
    if (ret != 0) {
        printf("SPI MMC version check failed.\n");
        goto error;
    }

error:
    return ret;
}

int spi_mmc_probe(struct mmc_dev *dev)
{
    if (dev->initialized == true) {
        return 0;
    }

    int ret = spi_mmc_init(dev);
    if (!ret) {
        dev->initialized = true;
    }
    return ret;
}

void spi_mmc_dbg_printbuf(struct mmc_dev *dev, uint8_t *buf)
{
    uint32_t i;
    for (i = 0; i < dev->num_blocks; ++i) {
        if (i % 16 == 15)
            printf("%02x\n", buf[i]);
        else
            printf("%02x ", buf[i]);
    }
}

struct mmc_drv *spi_mmc_drv_init(spi_inst_t *spi, uint cs_pin, bool use_dma)
{
    struct spi_mmc_dev_data *data = (struct spi_mmc_dev_data *)
        calloc(1, sizeof(struct spi_mmc_dev_data));
    if (!data) {
        printf("Could not allocate SPI MMC data: %d\n", -ENOMEM);
        return NULL;
    }
    data->use_dma = use_dma;
    data->spi = spi;
    data->cs_pin = cs_pin;

    struct mmc_dev *dev = (struct mmc_dev *)calloc(1, sizeof(struct mmc_dev));
    if (!dev) {
        printf("Could not allocate SPI MMC device: %d\n", -ENOMEM);
        goto free_data;
    }
    dev->name = "spi_mmc";
    dev->priv = data;

    struct mmc_ops *ops = (struct mmc_ops *)malloc(sizeof(struct mmc_ops));
    if (!ops) {
        printf("Could not allocate SPI MMC ops: %d\n", -ENOMEM);
        goto free_dev;
    }
    ops->probe = &spi_mmc_probe;
    ops->blk_read = NULL; // TODO
    ops->blk_write = NULL; // TODO
    ops->blk_erase = NULL; // TODO

    struct mmc_drv *drv = calloc(1, sizeof(struct mmc_drv));
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

void spi_mmc_drv_deinit(struct mmc_drv *drv)
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
