/**
 * @file spi_mmc.c
 * @brief Sources for MMC/SD I/O via SPI
 * @note This file contains the source code for MMC/SD I/O via SPI.
 * The source code is derived from various open source projects:
 * 
 * @ref https://github.com/torvalds/linux
 * @ref https://github.com/u-boot/u-boot
 * @ref https://github.com/arduino-libraries/SD
 * @ref https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
 *
 * Author: Maximilian Stephan @ Auxspace e.V.
 * Copyright (C) 2025 Auxspace e.V.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>

#include "pico/stdlib.h"
#include "pico/binary_info.h"
#include "pico/malloc.h"

#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/spi.h"

#include <aurora/compiler.h>
#include <aurora/list.h>
#include <aurora/spi.h>
#include <aurora/drivers/mmc/mmc.h>
#include <aurora/drivers/mmc/spi_mmc.h>

/*----------------------------------------------------------------------------*/

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
            return 1;  // At least 1 byte has to be read
    }
}

/*----------------------------------------------------------------------------*/

static void spi_mmc_go_low_frequency(struct spi_mmc_context *ctx)
{
    // Actual frequency: 398089
    spi_set_baudrate(ctx->spi->hw_spi, 400 * 1000);
}

/*----------------------------------------------------------------------------*/

static int spi_mmc_transfer(struct spi_mmc_context *ctx, const uint8_t *tx,
                            uint8_t* rx, size_t length)
{
    uint cs_pin = ctx->cs_pin;
    int len;

    // assert(512 == length || 1 == length);
    assert(tx || rx);
    // assert(!(tx && rx));

    if (ctx->spi->use_dma) {
        return spi_transfer_dma(ctx->spi, tx, rx, length);
    } else {
        len = spi_write_read_blocking(ctx->spi->hw_spi, tx, rx, length);
        return len == length ? 0 : -EIO;
    }
}

/*----------------------------------------------------------------------------*/

static int spi_mmc_wait_ready(struct spi_mmc_context *ctx)
{
    const uint32_t max_r = 10;

    uint8_t ret;
    uint8_t resp;
    uint8_t dummy = 0xFF;
    int i;

    for(i = 0; i < max_r; ++i) {
        // resp = sd_spi_write(spi, 0xFF);
        ret = spi_mmc_transfer(ctx, &dummy, &resp, 1);
        if (!(ret & R1_SPI_ERROR) && (resp != 0x00)) {
            break;
        }
    }

    if (resp == 0x00)
        printf("%s failed\r\n", __FUNCTION__);

    // Return success/failure
    if ((resp > 0x00) && (resp != 0xFF)) {
        return -EIO;
    }

    return 0;
}

/*----------------------------------------------------------------------------*/

static uint8_t spi_send_cmd(struct spi_mmc_context *ctx,
                            struct spi_mmc_message msg, uint8_t *rx)
{
    const size_t packet_size = 6;
    const uint max_retries = 0x10;

    uint8_t response = 0xff;
    const size_t resp_size = spi_mmc_resp_size(mmc_cmd_resp_type(msg.cmd));
    uint8_t tx = 0xff;
    char cmdPacket[packet_size];

    // Prepare the command packet
    cmdPacket[0] = (msg.start << 6) | msg.cmd;
    cmdPacket[1] = (msg.arg >> 24);
    cmdPacket[2] = (msg.arg >> 16);
    cmdPacket[3] = (msg.arg >> 8);
    cmdPacket[4] = (msg.arg >> 0);

#if SD_CRC_ENABLED
    if (crc_on) {
        cmdPacket[5] = (crc7(cmdPacket, 5) << 1) | msg.stop;
    } else
#endif
    {
        cmdPacket[5] = (msg.crc7 << 1) | msg.stop;
    }
    // send a command
    for (int i = 0; i < packet_size; i++) {
        spi_mmc_transfer(ctx, &cmdPacket[i], &response, 1);
    }

    // The received byte immediataly following CMD12 is a stuff byte,
    // it should be discarded before receive the response of the CMD12.
    // if (MMC_CMD_STOP_TRANSMISSION == msg.cmd) {
    //     spi_mmc_transfer(ctx, &tx, &response, 1);
    // }

    // Loop for response: Response is sent back within command response time
    // (NCR), 0 to 8 bytes for SDC
    memset(rx, 0xff, spi_mmc_resp_size(resp_size));
    for (int i = 0; i < max_retries; i++) {
        spi_mmc_transfer(ctx, &tx, &response, 1);
        // Got the response
        if (!(response & R1_SPI_ERROR)) {
            // parse the rest of the response
            rx[0] = response;
            if (resp_size > 1) {
                printf("Total response:\n\tr[0] = 0x%02x\n", rx[0]);
            }
            for(int j = 1; j < resp_size; j++) {
                spi_mmc_transfer(ctx, &tx, &rx[j], 1);
                printf("\tr[%d] = 0x%02x\n", j, rx[j]);
            }
            break;
        }
    }
    // R1 part of response
    return response;
}

/*----------------------------------------------------------------------------*/

static int send_msg(struct spi_mmc_context *ctx,
                    const struct spi_mmc_message msg, uint8_t* resp)
{
    /**
     * @note: no nullptr error handling done here for better performance.
     * Make sure values are valid before calling send_msg!
     */
    uint8_t ret;

    cs_select(ctx->cs_pin);
    ret = spi_send_cmd(ctx, msg, resp);
    cs_deselect(ctx->cs_pin);

    return 0;
}

/*----------------------------------------------------------------------------*/

static int send_reset(struct spi_mmc_context *ctx)
{
    int ret = 0;
    int i;
    const struct spi_mmc_message reset_cmd = SPI_MMC_CMD_CRC(
                                                MMC_CMD_GO_IDLE_STATE, 0, 0x4A);

    uint8_t resp = 0xff;
    for (i = 0; i < 10; i++) {
        ret = send_msg(ctx, reset_cmd, &resp);
        if (R1_SPI_IDLE == resp) {
            break;
        }
        for (int j = 0; j < 10000000; j++) {
            asm volatile("nop \n");
        }
    }

    if (resp & R1_SPI_ERROR) {
        printf("Sending reset command failed: %d\n", ret);
        ret = -EIO;
    } else if (unlikely(!(resp & R1_SPI_IDLE))) {
        printf("MMC SPI reset command timed out.\n");
        ret = -ETIMEDOUT;
    }

    return ret;
}

/*----------------------------------------------------------------------------*/

static int spi_mmc_voltage_select(struct mmc_dev *dev)
{
    const uint max_num_retries = 10;
    uint num_retries = 0;
    uint8_t response = 0xff;
    uint8_t *cmd8_resp;
    int ret;
    struct spi_mmc_context *ctx = (struct spi_mmc_context *)dev->priv;
    struct spi_mmc_message acmd41 = SPI_MMC_CMD(SD_CMD_APP_SEND_OP_COND, 0);
    const struct spi_mmc_message cmd8 = SPI_MMC_CMD_CRC(
                                            MMC_CMD_SEND_EXT_CSD, 0x1AA, 0x43);
    const struct spi_mmc_message cmd55 = SPI_MMC_CMD(MMC_CMD_APP_CMD, 0);
    const struct spi_mmc_message cmd58 = SPI_MMC_CMD(MMC_CMD_SPI_READ_OCR, 0);

    cmd8_resp = malloc(spi_mmc_resp_size(mmc_cmd_resp_type(cmd8.cmd)));
    ret = send_msg(ctx, cmd8, cmd8_resp);
    if (cmd8_resp[0] & R1_SPI_ILLEGAL_COMMAND) {
        dev->version = SD_CARD_TYPE_SD1;
    // only need last byte of r7 response (echo-back)
    } else if(cmd8_resp[4] & 0xAA) {
        dev->version = SD_CARD_TYPE_SD2;
    } else {
        printf("Card did not respond to voltage select.\n");
        return -EIO;
    }

    // set acmd41 arg depending on version
    acmd41.arg = dev->version == SD_CARD_TYPE_SD2 ? 0X40000000 : 0;

    response = 0xff;
    while(response == 0xff) {
        response = 0xff;
        ret = send_msg(ctx, cmd55, &response);
        if (ret) {
            printf("Sending CMD55 failed.\n");
            return -EIO;
        }

        response = 0xff;
        ret = spi_mmc_wait_ready(ctx);
        if (ret) {
            printf("Waiting for card to be ready failed.\n");
            continue;
        }

        ret = send_msg(ctx, acmd41, &response);
        if (ret) {
            printf("Sending ACMD41 failed.\n");
            return -EIO;
        } else if(num_retries++ >= max_num_retries) {
            printf("Sending ACMD41 timed out.\n");
            return -ETIMEDOUT;
        }
    }

    // if SD2 read OCR register to check for SDHC card
    if (dev->version == SD_CARD_TYPE_SD2) {
        response = 0xff;
        if (send_msg(ctx, cmd58, &response)) {
            printf("Sending CMD58 failed.\n");
            return -EIO;
        }
        if (!(response & R1_SPI_ERROR) && response & 0XC0) {
            dev->version = SD_CARD_TYPE_SDHC;
        }
        // discard rest of ocr - contains allowed voltage range
    }

    return ret;
}

/*----------------------------------------------------------------------------*/

static int spi_mmc_init(struct mmc_dev *dev)
{
    struct spi_mmc_message spi_init_message = { 0 };

    int ret;
    uint i;
    uint8_t resp = 0xff;
    struct spi_mmc_context *ctx = (struct spi_mmc_context *)dev->priv;
    if (!ctx) {
        printf("No SPI MMC ctx available!\n");
        return -EINVAL;
    }

    spi_mmc_go_low_frequency(ctx);

    // Wait for at least 74 cycles with MOSI and CS asserted
    memset(&spi_init_message, 0xff, sizeof(spi_init_message));
    for (i = 0; i < (74 / (sizeof(spi_init_message) * 8) + 1); i++) {
        send_msg(ctx, spi_init_message, &resp);
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
    send_reset(ctx);
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
    int ret;

    /* Make sure init is only run once */
    auto_init_mutex(sd_init_driver_mutex);
    mutex_enter_blocking(&sd_init_driver_mutex);

    if (dev->initialized == true) {
        ret = 0;
        goto out;
    }

    ret = spi_mmc_init(dev);
    if (!ret) {
        dev->initialized = true;
    }

out:
    mutex_exit(&sd_init_driver_mutex);
    return ret;
}

/*----------------------------------------------------------------------------*/

struct mmc_drv *spi_mmc_drv_init(struct spi_config *spi, uint cs_pin)
{
    auto_init_mutex(spi_mmc_drv_init_mutex);
    mutex_enter_blocking(&spi_mmc_drv_init_mutex);

    gpio_put(cs_pin, 1);  // Avoid any glitches when enabling output
    gpio_init(cs_pin);
    gpio_set_dir(cs_pin, GPIO_OUT);
    gpio_put(cs_pin, 1);  // In case set_dir does anything

    if (!aurora_spi_init(spi)) {
        goto out;
    }

    struct spi_mmc_context *ctx = (struct spi_mmc_context *)
        calloc(1, sizeof(struct spi_mmc_context));
    if (!ctx) {
        printf("Could not allocate SPI MMC ctx: %d\n", -ENOMEM);
        goto out;
    }
    ctx->spi = spi;
    ctx->cs_pin = cs_pin;

    // TODO: Figure this out
    // bi_decl(bi_1pin_with_name(cs_pin, "MMC SPI CS"));

    struct mmc_dev *dev = (struct mmc_dev *)calloc(1, sizeof(struct mmc_dev));
    if (!dev) {
        printf("Could not allocate SPI MMC device: %d\n", -ENOMEM);
        goto free_ctx;
    }
    dev->name = "spi_mmc";
    dev->priv = ctx;

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

    mutex_exit(&spi_mmc_drv_init_mutex);
    return drv;

free_ops:
    free(ops);
free_dev:
    free(dev);
free_ctx:
    free(ctx);
out:
    mutex_exit(&spi_mmc_drv_init_mutex);
    return NULL;
}

/*----------------------------------------------------------------------------*/

void spi_mmc_drv_deinit(struct mmc_drv *drv)
{
    if (!drv) {
        return;
    }

    if (!drv->dev) {
        goto free_ops;
    }

    if (drv->dev->priv) {
        aurora_spi_deinit((struct spi_config *)drv->dev->priv);
        free(drv->dev->priv);
    }

    free(drv->dev);

free_ops:
    if (drv->ops)
        free(drv->ops);

    free(drv);
}

/* [] END OF FILE */