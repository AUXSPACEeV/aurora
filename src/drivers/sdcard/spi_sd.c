/**
* @file spi_sd.c
* @brief Sources for micro SDCard I/O via SPI
* @note This file contains the source code for micro SDCard I/O via SPI.
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

#include <aurora/drivers/sdcard/spi_sd.h>

#define SPI_SDCARD_CMD_PAGE_PROGRAM 0x02
#define SPI_SDCARD_CMD_READ         0x03
#define SPI_SDCARD_CMD_STATUS       0x05
#define SPI_SDCARD_CMD_WRITE_EN     0x06
#define SPI_SDCARD_CMD_SECTOR_ERASE 0x20

#define SPI_SDCARD_STATUS_BUSY_MASK 0x01

#define SPI_INIT_CYCLES 74

/**
 * @brief SPI SDCard command structure (48 bits)
 * @note This structure is used to send commands to the SDCard.
 * @note The structure is packed to ensure that the data is aligned correctly.
 * https://www.dejazzer.com/ee379/lecture_notes/lec12_sd_card.pdf
 */
struct spi_sd_message {
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
} __attribute__((packed));

static inline void cs_select(uint cs_pin) {
    asm volatile("nop \n nop \n nop"); // FIXME
    gpio_put(cs_pin, 0);
    asm volatile("nop \n nop \n nop"); // FIXME
}

static inline void cs_deselect(uint cs_pin) {
    asm volatile("nop \n nop \n nop"); // FIXME
    gpio_put(cs_pin, 1);
    asm volatile("nop \n nop \n nop"); // FIXME
}

static int send_msg(spi_inst_t *spi,
    uint cs_pin, const struct spi_sd_message *msg)
{
    int ret;
    cs_select(cs_pin);
    ret = spi_write_blocking(spi, (uint8_t *)msg, sizeof(*msg));
    cs_deselect(cs_pin);
    if (ret != sizeof(*msg)) {
        printf("Error: %d\n", ret);
        return -EIO;
    }
    return 0;
}

static int send_reset(spi_inst_t *spi, uint cs_pin) {
    const struct spi_sd_message reset_cmd = {
            .start = 0b01,
            .cmd = 0,
            .arg = 0,
            .crc32_le = 0b1001010,
            .stop = 1,
    };

    return send_msg(spi, cs_pin, &reset_cmd);
}

int spi_sd_init (spi_inst_t *spi, uint cs_pin) {
    /**
     * First, put the SDCard into SPI mode by setting CS and MOSI to
     * logical HIGH for AT LEAST (may be more) SPI_INIT_CYCLES clock cycles.
     */
    int ret;
    const int spi_init_size =
        (SPI_INIT_CYCLES / sizeof(uint8_t)) * sizeof(uint8_t);
    uint8_t *spi_init_message = (uint8_t *) malloc(spi_init_size);

    if (!spi_init_message) {
        printf("Could not allocate spi init message: %d\n", ret);
        return -ENOMEM;
    }

    /* Set everything to "1" so the MOSI line is HIGH all the time */
    memset(spi_init_message, 1, sizeof(*spi_init_message));

    cs_select(cs_pin);
    ret = spi_write_blocking(spi, spi_init_message, sizeof(*spi_init_message));

    /* Set CS to 0 and send RESET */
    cs_deselect(cs_pin);
    if (ret != sizeof(*spi_init_message)) {
        goto free_message;
    }
    send_reset(spi, cs_pin);

free_message:
    free(spi_init_message);
    return ret;
}

void spi_sd_read (spi_inst_t *spi, uint cs_pin, uint32_t addr, uint8_t *buf, size_t len) {
    uint8_t cmdbuf[4] = {
        SPI_SDCARD_CMD_READ,
        addr >> 16,
        addr >> 8,
        addr
    };
    cs_select(cs_pin);
    spi_write_blocking(spi, cmdbuf, 4);
    spi_read_blocking(spi, 0, buf, len);
    cs_deselect(cs_pin);
}

void spi_sd_write_enable (spi_inst_t *spi, uint cs_pin) {
    cs_select(cs_pin);
    uint8_t cmd = SPI_SDCARD_CMD_WRITE_EN;
    spi_write_blocking(spi, &cmd, 1);
    cs_deselect(cs_pin);
}

void spi_sd_wait_done (spi_inst_t *spi, uint cs_pin) {
    uint8_t status;
    do {
        cs_select(cs_pin);
        uint8_t buf[2] = {SPI_SDCARD_CMD_STATUS, 0};
        spi_write_read_blocking(spi, buf, buf, 2);
        cs_deselect(cs_pin);
        status = buf[1];
    } while (status & SPI_SDCARD_STATUS_BUSY_MASK);
}

void spi_sd_sector_erase (spi_inst_t *spi, uint cs_pin, uint32_t addr) {
    uint8_t cmdbuf[4] = {
            SPI_SDCARD_CMD_SECTOR_ERASE,
            addr >> 16,
            addr >> 8,
            addr
    };
    spi_sd_write_enable(spi, cs_pin);
    cs_select(cs_pin);
    spi_write_blocking(spi, cmdbuf, 4);
    cs_deselect(cs_pin);
    spi_sd_wait_done(spi, cs_pin);
}

void spi_sd_page_program (spi_inst_t *spi, uint cs_pin, uint32_t addr, uint8_t data[]) {
    uint8_t cmdbuf[4] = {
            SPI_SDCARD_CMD_PAGE_PROGRAM,
            addr >> 16,
            addr >> 8,
            addr
    };
    spi_sd_write_enable(spi, cs_pin);
    cs_select(cs_pin);
    spi_write_blocking(spi, cmdbuf, 4);
    spi_write_blocking(spi, data, SPI_SDCARD_PAGE_SIZE);
    cs_deselect(cs_pin);
    spi_sd_wait_done(spi, cs_pin);
}

void spi_sd_dbg_printbuf(uint8_t buf[SPI_SDCARD_PAGE_SIZE]) {
    for (int i = 0; i < SPI_SDCARD_PAGE_SIZE; ++i) {
        if (i % 16 == 15)
            printf("%02x\n", buf[i]);
        else
            printf("%02x ", buf[i]);
    }
}
