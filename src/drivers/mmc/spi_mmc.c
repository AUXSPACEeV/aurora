/**
 * @file spi_mmc.c
 * @brief Sources for MMC/SD I/O via SPI
 * @note This file contains the source code for MMC/SD I/O via SPI.
 * The source code is derived from various open source projects:
 *
 * @ref https://github.com/torvalds/linux
 * @ref https://github.com/u-boot/u-boot
 * @ref https://github.com/arduino-libraries/SD
 * @ref https://github.com/carlk3/no-OS-FatFS-SD-SPI-RPi-Pico
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
#include <aurora/config.h>
#include <aurora/drivers/mmc/mmc.h>
#include <aurora/drivers/mmc/spi_mmc.h>
#include <aurora/drivers/spi.h>
#include <aurora/list.h>
#include <aurora/log.h>

/* Prototypes */

/**
 * @brief extract bits from abyte array
 *
 * @param data: byte array pointer
 * @param msb: Most significant bit to extract in data array
 * @param lsb: Least significant bit to extract in data array
 * @return: Bits between lsb and msb, shifted and masked accordingly
 *
 * @note Use if the bitfield to extract may span multiple bytes, possibly even
 * non-aligned.
 */
static uint32_t ext_bits(uint8_t *data, int msb, int lsb);

/**
 * @brief Enable low frequency SPI mode for SDCard
 *
 * @param ctx: pointer to spi mmc driver context
 */
static void spi_mmc_go_low_frequency(struct spi_mmc_context *ctx);

/**
 * @brief Initialize the mmc driver data and context
 *
 * @param ctx: pointer to spi mmc driver context
 * @return: Error code
 */
static int spi_mmc_init(struct mmc_dev *dev);

/**
 * @brief Send an SPI MMC command to the SDCard (raw)
 *
 * @param ctx: pointer to spi mmc driver context
 * @param msg: message to send
 * @param rx: SPI transfer receive buffer. Has to be sized so that the SPI
 * response fits
 * @param is_acmd: true if the command is an ACMD, false otherwise
 * @return: 0 on success, error code otherwise
 */
static int spi_mmc_send_cmd(struct spi_mmc_context *ctx,
                            struct spi_mmc_message msg, uint8_t *rx,
                            bool is_acmd);

/**
 * @brief get the sizie in bytes for an MMC response type
 *
 * @param resp_type: mmc response type
 * @return: size of the response in bytes
 */
static size_t spi_mmc_resp_size(mmc_response_t resp_type);

/**
 * @brief Get the number of sectors on an SD card.
 *
 * @param dev: pointer to spi mmc device driver
 * @return: number of sectors on the card, or 0 if an error occurred.
 *
 * @details This function sends a CMD9 command to the card to get the Card
 * Specific Data (CSD) and then extracts the number of sectors from the CSD.
 */
static uint64_t spi_mmc_sectors(struct mmc_dev *dev);

/**
 * @brief Send a message via spi
 *
 * @param ctx: pointer to spi mmc driver context
 * @param msg: message to send
 * @param resp: SPI transfer receive buffer. Has to be sized so that the SPI
 * response fits
 * @return: Error code
 */
static int spi_mmc_send_msg(struct spi_mmc_context *ctx,
                            const struct spi_mmc_message msg, uint8_t* resp);

/**
 * @brief send card reset command
 *
 * @param ctx: pointer to spi mmc driver context
 * @return: Error code
 */
static int spi_mmc_send_reset(struct spi_mmc_context *ctx);

/**
 * @brief Do an SPI transfer
 *
 * @param ctx: pointer to spi mmc driver context
 * @param tx: SPI write buffer
 * @param rx: SPI receive buffer
 * @param length: total length of SPI transfer in words (here: bytes)
 * @return: Error code
 */
static int spi_mmc_transfer(struct spi_mmc_context *ctx, const uint8_t *tx,
                            uint8_t* rx, size_t length);

/**
 * @brief Send the voltage select command to the card
 *
 * @param dev: pointer to spi mmc device driver
 * @return: Error code
 */
static int spi_mmc_voltage_select(struct mmc_dev *dev);

/**
 * @brief Wait for the card to be ready
 *
 * @param dev: pointer to spi mmc device driver
 * @return: Error code
 *
 * TODO: add timeout via timer/rtc instead of fixed retries
 */
static int spi_mmc_wait_ready(struct spi_mmc_context *ctx);

/**
 * @brief read blocks from the spi mmc/sd card
 *
 * @param dev: mmc device struct
 * @param blk: block number to read
 * @param buf: readback buffer
 * @param len: number of blocks to read
 * @return: Error code
 */
static int spi_mmc_read_blocks(struct mmc_dev *dev, uint32_t blk, uint8_t *buf,
                               const uint32_t len);

/**
 * @brief Probe function for the driver
 *
 * @param dev: mmc device struct to probe
 * @return: Error code
 */
static int spi_mmc_probe(struct mmc_dev *dev);

/* Driver function implementation */

static uint32_t ext_bits(uint8_t *data, int msb, int lsb) {
    uint32_t bits = 0;
    uint32_t size = 1 + msb - lsb;
    for (uint32_t i = 0; i < size; i++) {
        uint32_t position = lsb + i;
        uint32_t byte = 15 - (position >> 3);
        uint32_t bit = position & 0x7;
        uint32_t value = (data[byte] >> bit) & 1;
        bits |= value << i;
    }
    return bits;
}

const static struct mmc_ops drv_ops = {
    .probe = &spi_mmc_probe,
    .blk_read = &spi_mmc_read_blocks,
    .blk_write = NULL, // TODO
    .blk_erase = NULL, // TODO
    .generate_info = NULL, // TODO
    .n_sectors = &spi_mmc_sectors,
};

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

static void spi_mmc_resume_frequency(struct spi_mmc_context *ctx)
{
    spi_set_baudrate(ctx->spi->hw_spi, ctx->spi->baud_rate);
}

/*----------------------------------------------------------------------------*/

static int spi_mmc_transfer(struct spi_mmc_context *ctx, const uint8_t *tx,
                            uint8_t* rx, size_t length)
{
    int len;
    int ret = 0;

    // assert(512 == length || 1 == length);
    assert(tx || rx);
    // assert(!(tx && rx));

    cs_select(ctx->cs_pin);

    if (ctx->spi->use_dma) {
        ret = spi_transfer_dma(ctx->spi, tx, rx, length);
    } else {
        len = spi_write_read_blocking(ctx->spi->hw_spi, tx, rx, length);
        if (len != length) {
            log_error("SPI transfer failed: %d != %d\r\n", len, length);
            ret = -EIO;
        }
    }

    cs_deselect(ctx->cs_pin);

    return ret;
}

/*----------------------------------------------------------------------------*/

static int spi_mmc_wait_ready(struct spi_mmc_context *ctx)
{
    log_trace("%s()\r\n", __FUNCTION__);
    const uint32_t max_r = 10;

    uint8_t ret;
    uint8_t resp;
    uint8_t dummy = 0xFF;
    int i;

    for(i = 0; i < max_r; ++i) {
        ret = spi_mmc_transfer(ctx, &dummy, &resp, 1);
        if (resp != 0x00) {
            break;
        }
    }

    if (resp == 0x00) {
        log_error("%s failed\r\n", __FUNCTION__);
    }

    // Return success/failure
    if ((resp > 0x00) && (resp != 0xFF)) {
        return -EIO;
    }

    return 0;
}

/*----------------------------------------------------------------------------*/

static int spi_mmc_send_cmd(struct spi_mmc_context *ctx,
                                struct spi_mmc_message msg, uint8_t *rx,
                                bool is_acmd)
{
    log_trace("%s({.cmd = 0x%02lx, .arg = 0x%08lx, .crc7 = 0x%02lx}, 0x%08lx)",
              __FUNCTION__, msg.cmd, msg.arg, msg.crc7, (uintptr_t)rx);

    const size_t packet_size = 6;
    const uint max_retries = 0x10;
    const size_t resp_size = spi_mmc_resp_size(mmc_cmd_get_resp_type(msg.cmd));
    const struct spi_mmc_message cmd55 = SPI_MMC_CMD(MMC_CMD_APP_CMD, 0);

    int ret = 0;
    uint8_t response = 0xff;
    uint8_t dummy = 0xff;
    char cmdPacket[packet_size];

    if (is_acmd) {
        ret = spi_mmc_send_cmd(ctx, cmd55, &response, false);
        if (ret) {
            log_error("CMD55 failed: %02x\n", response);
            return -EIO;
        }
        response = 0xff;

        if (spi_mmc_wait_ready(ctx)) {
            log_error("Waiting for card to be ready failed.\n");
            return R1_SPI_ERROR;
        }
    }

    // Prepare the command packet
    cmdPacket[0] = (msg.start << 6) | msg.cmd;
    cmdPacket[1] = (msg.arg >> 24);
    cmdPacket[2] = (msg.arg >> 16);
    cmdPacket[3] = (msg.arg >> 8);
    cmdPacket[4] = (msg.arg >> 0);

#if IS_ENABLED(CONFIG_MMC_CRC_ENABLED)
    cmdPacket[5] = (crc7(cmdPacket, 5) << 1) | msg.stop;
#else
    cmdPacket[5] = (msg.crc7 << 1) | msg.stop;
#endif
    // send a command
    for (int i = 0; i < packet_size; i++) {
        spi_mmc_transfer(ctx, &cmdPacket[i], &response, 1);
    }

    // The received byte immediataly following CMD12 is a stuff byte,
    // it should be discarded before receive the response of the CMD12.
    if (MMC_CMD_STOP_TRANSMISSION == msg.cmd) {
        spi_mmc_transfer(ctx, &dummy, &response, 1);
    }

    // Loop for response: Response is sent back within command response time
    // (NCR), 0 to 8 bytes for SDC
    memset(rx, 0xff, resp_size);
    for (int i = 0; i < max_retries; i++) {
        spi_mmc_transfer(ctx, &dummy, &response, 1);
        // Got the response
        if (!(response & R1_SPI_ERROR)) {
            // parse the rest of the response
            rx[0] = response;
            for(int j = 1; j < resp_size; j++) {
                spi_mmc_transfer(ctx, &dummy, &rx[j], 1);
            }
            break;
        }
    }
    // R1 part of response
    log_debug("%sCMD%u: %02x\n", is_acmd ? "A" : "", msg.cmd, response);
    return (response & R1_SPI_ERROR) ? -EIO : 0;
}

/*----------------------------------------------------------------------------*/

static int spi_mmc_send_msg(struct spi_mmc_context *ctx,
                            const struct spi_mmc_message msg, uint8_t* resp)
{
    log_trace("%s({.cmd = 0x%02lx, .arg = 0x%08lx, .crc7 = 0x%02lx}, 0x%08lx)",
              __FUNCTION__, msg.cmd, msg.arg, msg.crc7, (uintptr_t)resp);

    return spi_mmc_send_cmd(ctx, msg, resp, false);
}

/*----------------------------------------------------------------------------*/

static uint64_t spi_mmc_sectors(struct mmc_dev *dev)
{
    log_trace("%s()\r\n", __FUNCTION__);
    // CMD9, Response R2 (R1 byte + 16-byte block read)
    const struct spi_mmc_message cmd9 = SPI_MMC_CMD(MMC_CMD_SEND_CSD, 0);
    const size_t resp_size = mmc_get_resp_size(
                                mmc_cmd_get_resp_type(cmd9.cmd));
    const uint8_t tx_dummy = 0xff;

    struct spi_mmc_context *ctx = (struct spi_mmc_context *)dev->priv;
    int ret = 0;
    uint32_t csd_structure;
    uint32_t c_size, c_size_mult, read_bl_len;
    uint32_t block_len, mult, blocknr;
    uint32_t hc_c_size;
    uint64_t blocks = 0, capacity = 0;
    uint8_t csd[16] = {0};
    uint8_t *resp;
    size_t i;

    spi_lock(ctx->spi);
    resp = malloc(resp_size);
    memset(resp, 0xff, resp_size);

    /* Send CMD 9 to request 16-byte block of CSD register data */
    ret = spi_mmc_send_cmd(ctx, cmd9, resp, false);
    /* resp (R2) has to be valid before reading 16-byte block */
    if (ret) {
        log_error("CMD9 failed: %d\n", ret);
        log_debug("Full CMD9 response:\n", resp[0]);
        for (i = 0; i < resp_size; i++)
        log_debug("%u: %02x\n", i, resp[i]);
        goto free_response;
    }

    /* Read 16-byte block of CSD register data after R2 from CMD9 */
    ret = spi_mmc_transfer(ctx, &tx_dummy, csd, sizeof(csd));
    if (ret) {
        log_error("Couldn't read CSD response from disk\n");
        goto free_response;
    }

    /**
     * Using ext_bits instead of simple shifting and masking because data
     * array is too big to fit inside a single type.
     * This implies big-endian bit layout, but reversed at the byte level.
     */
    csd_structure = ext_bits(csd, 126, 127);
    switch (csd_structure) {
        case 0:
            c_size = ext_bits(csd, 73, 62);       // c_size        : csd[73:62]
            c_size_mult = ext_bits(csd, 49, 47);  // c_size_mult   : csd[49:47]
            read_bl_len =
                ext_bits(csd, 83, 80);     // read_bl_len   : csd[83:80] - the
                                           // *maximum* read block length
            block_len = 1 << read_bl_len;  // BLOCK_LEN = 2^READ_BL_LEN
            mult = 1 << (c_size_mult +
                         2);         // MULT = 2^C_SIZE_MULT+2 (C_SIZE_MULT < 8)
            blocknr = (c_size + 1) * mult;  // BLOCKNR = (C_SIZE+1) * MULT
            capacity = (uint64_t)blocknr *
                       block_len;  // memory capacity = BLOCKNR * BLOCK_LEN
            blocks = capacity / (uint64_t)dev->blksize;
            log_debug("Standard Capacity: c_size: %lu\r\n", c_size);
            log_debug("Sectors: 0x%llx : %llu\r\n", blocks, blocks);
            log_debug("Capacity: 0x%llx : %llu MB\r\n", capacity,
                       (uint64_t)(capacity / (1024U * 1024U)));
            break;
        case 1:
            hc_c_size =
                ext_bits(csd, 69, 48);       // device size : C_SIZE : [69:48]
            blocks = ((uint64_t)hc_c_size + 1) << 10;
                                             // block count = C_SIZE+1) * 1K
                                             // byte (512B is block size)
            log_debug("SDHC/SDXC Card: hc_c_size: %lu\r\n", hc_c_size);
            log_debug("Sectors: %16llu\r\n", blocks);
            log_debug("Capacity: %16llu MB\r\n", (uint64_t)(blocks / (2048U)));
            break;

        default:
            log_error("CSD struct unsupported\r\n");
    };

free_response:
    free(resp);
    dev->num_blocks = blocks;
    spi_unlock(ctx->spi);
    return blocks;
}

/*----------------------------------------------------------------------------*/

static int spi_mmc_wait_token(struct spi_mmc_context *ctx, uint8_t token) {
    log_trace("%s(0x%02hhx)\r\n", __FUNCTION__, token);

    const uint32_t timeout = SD_COMMAND_TIMEOUT;  // Wait for start token
    const uint8_t dummy = 0xff;
    uint8_t resp = 0xff;
    absolute_time_t timeout_time = make_timeout_time_ms(timeout);
    int __attribute__((unused)) ret;

    do {
        ret = spi_mmc_transfer(ctx, &dummy, &resp, 1);
        if (token == resp) {
            return 0;
        }
    } while (0 < absolute_time_diff_us(get_absolute_time(), timeout_time));
    log_error("sd_wait_token: timeout\r\n");
    return -ETIMEDOUT;
}

/*----------------------------------------------------------------------------*/

static int __spi_mmc_read_block(struct mmc_dev *dev, uint8_t *buf,
                                const uint32_t len)
{
    log_trace("%s(0x%08lx, 0x%016llx)\r\n", __FUNCTION__, (uintptr_t)buf,
              len);
    const uint8_t dummy = 0xff;
    uint8_t resp = 0xff;
    uint16_t crc;
    int ret;
    struct spi_mmc_context *ctx = (struct spi_mmc_context *)dev->priv;

    // read until start byte (0xFE)
    ret = spi_mmc_wait_token(ctx, SPI_MMC_START_BLOCK);
    if (ret) {
        log_error("%s:%d Read timeout\r\n", __FILE__, __LINE__);
        return ret;
    }
    // read data
    ret = spi_mmc_transfer(ctx, &dummy, buf, (size_t)len);
    if (ret) {
        log_error("Reading %ld blocks failed: %d\n", len, ret);
        return ret;
    }
    // Read the CRC16 checksum for the data block
    ret = spi_mmc_transfer(ctx, &dummy, &resp, 1);
    crc = resp << 8;
    ret = spi_mmc_transfer(ctx, &dummy, &resp, 1);
    crc |= resp;

#if IS_ENABLED(CONFIG_MMC_CRC_ENABLED)
    uint32_t crc_result;
    // Compute and verify checksum
    crc_result = crc16((void *)buf, len);
    if ((uint16_t)crc_result != crc) {
        log_error("%s: Invalid CRC received 0x%04x"
                    " result of computation 0x%04x\r\n",
                    __FUNCTION__, crc, (uint16_t)crc_result);
        return -EBADMSG;
    }
#endif

    return 0;
}

/*----------------------------------------------------------------------------*/

static int spi_mmc_read_blocks(struct mmc_dev *dev, uint32_t blk, uint8_t *buf,
                               const uint32_t len)
{
    log_trace("%s(0x%016llx, 0x%08lx, 0x%016llx)\r\n", __FUNCTION__, blk,
              (uintptr_t)buf, len);
    uint32_t blockCnt = (uint32_t)len;  // SPI cmd can only handle 32 bit
    struct spi_mmc_context *ctx = (struct spi_mmc_context *)dev->priv;
    struct spi_mmc_message read_cmd = SPI_MMC_CMD(
        len > 1 ? MMC_CMD_READ_MULTIPLE_BLOCK : MMC_CMD_READ_SINGLE_BLOCK, 0);
    struct spi_mmc_message cmd12 = SPI_MMC_CMD(MMC_CMD_STOP_TRANSMISSION, 0);
    uint8_t response = 0xff;
    int ret;

    if (blk + blockCnt > dev->num_blocks) {
        log_error("Cannot read %ld blocks from %ld: out of bounds (%ld)\n",
               blockCnt, blk, dev->num_blocks);
        return -EINVAL;
    }

    spi_lock(ctx->spi);

    if (!ctx->spi->state->initialized || !dev->initialized) {
        log_error("Cannot read SPI MMC blocks: Driver not initialized.");
        spi_unlock(ctx->spi);
        return -EHOSTDOWN;
    }

    // SDSC Card (CCS=0) uses byte unit address
    // SDHC and SDXC Cards (CCS=1) use block unit address (512 Bytes unit)
    if (dev->version == SD_CARD_TYPE_SDHC) {
        read_cmd.arg = blk;
    } else {
        read_cmd.arg = blk * dev->blksize;
    }
    // Write command to receive data
    ret = spi_mmc_send_cmd(ctx, read_cmd, &response, false);
    if (ret) {
        log_error("Got error while reading blocks from SD Card: %02x\n", response);
        spi_unlock(ctx->spi);
        return -EIO;
    }
    // receive the data : one block at a time
    int rd_status = 0;
    while (blockCnt) {
        if (__spi_mmc_read_block(dev, buf, dev->blksize)) {
            rd_status = -EIO;
            break;
        }
        buf += dev->blksize;
        --blockCnt;
    }
    // Send CMD12(0x00000000) to stop the transmission for multi-block transfer
    if (len > 1) {
        ret = spi_mmc_send_cmd(ctx, cmd12, &response, false);
    }

    spi_unlock(ctx->spi);
    return rd_status ? rd_status : ret;
}

/*----------------------------------------------------------------------------*/

static int spi_mmc_send_reset(struct spi_mmc_context *ctx)
{
    log_trace("%s()\r\n", __FUNCTION__);
    int ret = 0;
    int i;
    const struct spi_mmc_message reset_cmd = SPI_MMC_CMD_CRC(
                                                MMC_CMD_GO_IDLE_STATE, 0, 0x4A);

    uint8_t resp = 0xff;
    for (i = 0; i < 10; i++) {
        ret = spi_mmc_send_msg(ctx, reset_cmd, &resp);
        if (R1_SPI_IDLE == resp) {
            break;
        }
        for (int j = 0; j < 10000000; j++) {
            asm volatile("nop \n");
        }
    }

    if (resp & R1_SPI_ERROR) {
        log_error("Sending reset command failed: %d\n", ret);
        ret = -EIO;
    } else if (unlikely(!(resp & R1_SPI_IDLE))) {
        log_error("MMC SPI reset command timed out.\n");
        ret = -ETIMEDOUT;
    }

    return ret;
}

/*----------------------------------------------------------------------------*/

static int spi_mmc_voltage_select(struct mmc_dev *dev)
{
    log_trace("%s()\r\n", __FUNCTION__);
    const uint max_num_retries = 10;
    uint num_retries = 0;
    uint8_t response = 0xff;
    uint8_t *cmd8_resp;
    uint8_t ret;
    struct spi_mmc_context *ctx = (struct spi_mmc_context *)dev->priv;
    struct spi_mmc_message acmd41 = SPI_MMC_CMD(SD_CMD_APP_SEND_OP_COND, 0);
    const struct spi_mmc_message cmd8 = SPI_MMC_CMD_CRC(
                                            MMC_CMD_SEND_EXT_CSD, 0x1AA, 0x43);
    const struct spi_mmc_message cmd58 = SPI_MMC_CMD(MMC_CMD_SPI_READ_OCR, 0);

    cmd8_resp = malloc(spi_mmc_resp_size(mmc_cmd_get_resp_type(cmd8.cmd)));
    ret = spi_mmc_send_cmd(ctx, cmd8, cmd8_resp, false);
    if (ret || cmd8_resp[0] & R1_SPI_ILLEGAL_COMMAND) {
        dev->version = SD_CARD_TYPE_SD1;
    // only need last byte of r7 response (echo-back)
    } else if(cmd8_resp[4] & 0xAA) {
        dev->version = SD_CARD_TYPE_SD2;
    } else {
        log_error("Card did not respond to voltage select.\n");
        return -EIO;
    }

    // set acmd41 arg depending on version
    acmd41.arg = dev->version == SD_CARD_TYPE_SD2 ? 0X40000000 : 0;

    response = 0xff;
    while(response == 0xff) {
        response = 0xff;
        ret = spi_mmc_send_cmd(ctx, acmd41, &response, true);
        if (ret) {
            log_error("Sending ACMD41 failed.\n");
            return -EIO;
        } else if(num_retries++ >= max_num_retries) {
            log_error("Sending ACMD41 timed out.\n");
            return -ETIMEDOUT;
        }
        sleep_us(10);
    }

    // if SD2 read OCR register to check for SDHC card
    if (dev->version == SD_CARD_TYPE_SD2) {
        if (spi_mmc_send_msg(ctx, cmd58, cmd8_resp)) {
            log_error("Sending CMD58 failed.\n");
            return -EIO;
        }
        if (!(cmd8_resp[0] & R1_SPI_ERROR) && cmd8_resp[0] & 0XC0) {
            dev->version = SD_CARD_TYPE_SDHC;
        }
        // discard rest of ocr - contains allowed voltage range
    }

    return ret;
}

/*----------------------------------------------------------------------------*/

static int spi_mmc_init(struct mmc_dev *dev)
{
    log_trace("%s()\r\n", __FUNCTION__);
    struct spi_mmc_message spi_init_message = { 0 };

    int ret;
    uint i;
    uint8_t resp = 0xff;
    struct spi_mmc_context *ctx = (struct spi_mmc_context *)dev->priv;
    if (!ctx) {
        log_error("No SPI MMC ctx available!\n");
        return -EINVAL;
    }

    spi_lock(ctx->spi);
    spi_mmc_go_low_frequency(ctx);

    // Wait for at least 74 cycles with MOSI and CS asserted
    memset(&spi_init_message, 0xff, sizeof(spi_init_message));
    for (i = 0; i < ((74 / (sizeof(spi_init_message) * 8)) + 1); i++) {
        spi_mmc_send_msg(ctx, spi_init_message, &resp);
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
    spi_mmc_send_reset(ctx);
    ret = spi_mmc_voltage_select(dev);
    if (ret != 0) {
        log_error("SPI MMC version check failed.\n");
    }
    log_debug("SPI MMC version/type: %s\n", mmc_type_to_str(dev->version));

    spi_mmc_resume_frequency(ctx);
    spi_unlock(ctx->spi);

    return ret;
}

static int spi_mmc_probe(struct mmc_dev *dev)
{
    log_trace("%s()\r\n", __FUNCTION__);
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
    log_trace("%s(%u)\r\n", __FUNCTION__, cs_pin);
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
        log_error("Could not allocate SPI MMC ctx: %d\n", -ENOMEM);
        goto out;
    }
    ctx->spi = spi;
    ctx->cs_pin = cs_pin;

    // TODO: Figure this out
    // bi_decl(bi_1pin_with_name(cs_pin, "MMC SPI CS"));

    struct mmc_dev *dev = (struct mmc_dev *)calloc(1, sizeof(struct mmc_dev));
    if (!dev) {
        log_error("Could not allocate SPI MMC device: %d\n", -ENOMEM);
        goto free_ctx;
    }
    dev->name = "spi_mmc";
    dev->blksize = BLOCK_SIZE_SD;
    dev->priv = ctx;

    struct mmc_drv *drv = calloc(1, sizeof(struct mmc_drv));
    if (!drv) {
        log_error("Could not allocate SPI MMC driver: %d\n", -ENOMEM);
        goto free_dev;
    }
    drv->dev = dev;
    drv->ops = &drv_ops;

    mutex_exit(&spi_mmc_drv_init_mutex);
    return drv;

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
    log_trace("%s()\r\n", __FUNCTION__);
    auto_init_mutex(spi_mmc_drv_deinit_mutex);
    mutex_enter_blocking(&spi_mmc_drv_deinit_mutex);

    if (!drv) {
        goto out;
    }

    if (!drv->dev) {
        goto free_drv;
    }

    if (drv->dev->priv) {
        aurora_spi_deinit((struct spi_config *)drv->dev->priv);
        free(drv->dev->priv);
    }

    free(drv->dev);

free_drv:
    free(drv);

out:
    mutex_exit(&spi_mmc_drv_deinit_mutex);
}

/* [] END OF FILE */