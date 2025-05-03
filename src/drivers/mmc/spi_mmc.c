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
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/spi.h"
#include "errno.h"

#include <aurora/drivers/mmc/mmc.h>
#include <aurora/drivers/mmc/spi_mmc.h>

static bool irqChannel1 = false;
static bool irqShared = false;
static struct spi_drv *spi_drivers[2];

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

void set_spi_dma_irq_channel(bool useChannel1, bool shared) {
    irqChannel1 = useChannel1;
    irqShared = shared;
}

static int spi_get_num() {
    return sizeof(spi_drivers) / sizeof(spi_drivers[0]);
}

static struct spi_drv *spi_get_by_num(size_t num) {
    assert(num < spi_get_num());
    return spi_drivers[num];
}

static void in_spi_irq_handler(const uint DMA_IRQ_num, io_rw_32 *dma_hw_ints_p) {
    for (size_t i = 0; i < spi_get_num(); ++i) {
        struct spi_drv *spi_p = spi_get_by_num(i);
        if (spi_p == NULL) {
            continue;
        }
        if (DMA_IRQ_num == spi_p->DMA_IRQ_num)  {
            // Is the SPI's channel requesting interrupt?
            if (*dma_hw_ints_p & (1 << spi_p->rx_dma)) {
                *dma_hw_ints_p = 1 << spi_p->rx_dma;  // Clear it.
                assert(!dma_channel_is_busy(spi_p->rx_dma));
                assert(!sem_available(&spi_p->sem));
                bool ok = sem_release(&spi_p->sem);
                assert(ok);
            }
        }
    }
}

static void __not_in_flash_func(spi_irq_handler_0)() {
    in_spi_irq_handler(DMA_IRQ_0, &dma_hw->ints0);
}

static void __not_in_flash_func(spi_irq_handler_1)() {
    in_spi_irq_handler(DMA_IRQ_1, &dma_hw->ints1);
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
            return 1;  // At least 1 byte has to be read
    }
}

static void spi_lock(struct spi_drv *spi) {
    assert(mutex_is_initialized(&spi->mutex));
    mutex_enter_blocking(&spi->mutex);
}

static void spi_unlock(struct spi_drv *spi) {
    assert(mutex_is_initialized(&spi->mutex));
    mutex_exit(&spi->mutex);
}

static void spi_mmc_go_low_frequency(struct spi_mmc_dev_data *data) {
    // Actual frequency: 398089
    spi_set_baudrate(data->spi->hw_spi, 400 * 1000);
}

static int spi_mmc_transfer_dma(struct spi_mmc_dev_data *data, const uint8_t *tx,
                            uint8_t* rx, size_t length)
{
    // assert(512 == length || 1 == length);
    if (!tx && !rx) {
        return -EINVAL;
    }
    // assert(!(tx && rx));

    // tx write increment is already false
    if (tx) {
        channel_config_set_read_increment(&data->spi->tx_dma_cfg, true);
    } else {
        static const uint8_t dummy = 0xff;
        tx = &dummy;
        channel_config_set_read_increment(&data->spi->tx_dma_cfg, false);
    }

    // rx read increment is already false
    if (rx) {
        channel_config_set_write_increment(&data->spi->rx_dma_cfg, true);
    } else {
        static uint8_t dummy = 0xA5;
        rx = &dummy;
        channel_config_set_write_increment(&data->spi->rx_dma_cfg, false);
    }

    dma_channel_configure(data->spi->tx_dma, &data->spi->tx_dma_cfg,
                          &spi_get_hw(data->spi->hw_spi)->dr,  // write address
                          tx,                              // read address
                          length,  // element count (each element is of
                                   // size transfer_data_size)
                          false);  // start
    dma_channel_configure(data->spi->rx_dma, &data->spi->rx_dma_cfg,
                          rx,                              // write address
                          &spi_get_hw(data->spi->hw_spi)->dr,  // read address
                          length,  // element count (each element is of
                                   // size transfer_data_size)
                          false);  // start

    switch (data->spi->DMA_IRQ_num) {
        case DMA_IRQ_0:
            assert(!dma_channel_get_irq0_status(data->spi->rx_dma));
            break;
        case DMA_IRQ_1:
            assert(!dma_channel_get_irq1_status(data->spi->rx_dma));
            break;
        default:
            assert(false);
    }
    sem_reset(&data->spi->sem, 0);

    // start them exactly simultaneously to avoid races (in extreme cases
    // the FIFO could overflow)
    dma_start_channel_mask((1u << data->spi->tx_dma) | (1u << data->spi->rx_dma));

    /* Wait until master completes transfer or time out has occured. */
    uint32_t timeOut = 1000; /* Timeout 1 sec */
    bool rc = sem_acquire_timeout_ms(
        &data->spi->sem, timeOut);  // Wait for notification from ISR
    if (!rc) {
        // If the timeout is reached the function will return false
        printf("Notification wait timed out in %s\n", __FUNCTION__);
        return -ETIMEDOUT;
    }
    // Shouldn't be necessary:
    dma_channel_wait_for_finish_blocking(data->spi->tx_dma);
    dma_channel_wait_for_finish_blocking(data->spi->rx_dma);

    assert(!sem_available(&data->spi->sem));
    assert(!dma_channel_is_busy(data->spi->tx_dma));
    assert(!dma_channel_is_busy(data->spi->rx_dma));

    return 0;
}

static int spi_mmc_transfer(struct spi_mmc_dev_data *data, const uint8_t *tx,
    uint8_t* rx, size_t length)
{
    uint cs_pin = data->cs_pin;
    int len;

    // assert(512 == length || 1 == length);
    assert(tx || rx);
    // assert(!(tx && rx));

    if (data->spi->use_dma) {
        printf("dma\n");
        return spi_mmc_transfer_dma(data, tx, rx, length);
    } else {
        len = spi_write_read_blocking(data->spi->hw_spi, tx, rx, length);
        return len == length ? 0 : -EIO;
    }
}

static int spi_mmc_wait_ready(struct spi_mmc_dev_data *data) {
    const uint32_t max_r = 10;

    uint8_t ret;
    uint8_t resp;
    uint8_t dummy = 0xFF;
    int i;

    for(i = 0; i < max_r; ++i) {
        // resp = sd_spi_write(spi, 0xFF);
        ret = spi_mmc_transfer(data, &dummy, &resp, 1);
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

static uint8_t spi_send_cmd(struct spi_mmc_dev_data *data, struct spi_mmc_message msg, uint8_t *rx)
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
        cmdPacket[5] = (crc7(cmdPacket, 5) << 1) | 0x01;
    } else
#endif
    {
        cmdPacket[5] = (msg.crc7 << 1) | msg.stop;
    }
    // send a command
    for (int i = 0; i < packet_size; i++) {
        spi_mmc_transfer(data, &cmdPacket[i], &response, 1);
    }

    // The received byte immediataly following CMD12 is a stuff byte,
    // it should be discarded before receive the response of the CMD12.
    // if (MMC_CMD_STOP_TRANSMISSION == msg.cmd) {
    //     spi_mmc_transfer(data, &tx, &response, 1);
    // }

    // Loop for response: Response is sent back within command response time
    // (NCR), 0 to 8 bytes for SDC
    memset(rx, 0xff, spi_mmc_resp_size(resp_size));
    for (int i = 0; i < max_retries; i++) {
        spi_mmc_transfer(data, &tx, &response, 1);
        // Got the response
        if (!(response & R1_SPI_ERROR)) {
            // parse the rest of the response
            rx[0] = response;
            if (resp_size > 1) {
                printf("Total response:\n\tr[0] = 0x%02x\n", rx[0]);
            }
            for(int j = 1; j < resp_size; j++) {
                spi_mmc_transfer(data, &tx, &rx[j], 1);
                printf("\tr[%d] = 0x%02x\n", j, rx[j]);
            }
            break;
        }
    }
    printf("CMD: 0x%02x; ARG: 0x%08x; R1: 0x%02x\n", msg.cmd, msg.arg, response);
    // R1 part of response
    return response;
}

// static int send_msg(struct spi_mmc_dev_data *data,
//                     const struct spi_mmc_message *msg, uint8_t* resp,
//                     mmc_response_t resp_type)
// {
//     /**
//      * @note: no nullptr error handling done here for better performance.
//      * Make sure values are valid before calling send_msg!
//      */
//     int ret;
//     uint8_t *dst;
//     uint8_t tmp;
//     uint i;
//     uint cs_pin = data->cs_pin;
//     spi_inst_t *spi = data->spi->hw_spi;

//     cs_select(cs_pin);
//     dst = (uint8_t *)calloc(1, sizeof(uint8_t));
//     if (!dst) {
//         printf("Could not allocate response buffer: %d\n", -ENOMEM);
//         return -ENOMEM;
//     }

//     // Write and read separately, since SDCard can take a while to respond
//     ret = spi_write_blocking(spi, (uint8_t *)msg, sizeof(*msg));
//     uint64_t val = 0;
//     memcpy(&val, msg, sizeof(*msg)); // Safely copy struct bytes into uint64_t
//     printf("\nMSG: 0x%016llx\n", val);
//     for (i = 0; i < 0XFF; i++) {
//         ret |= spi_read_blocking(spi, 0xFF, dst, sizeof(uint8_t));
//         /**
//          * The first response byte of every command is R1.
//          * R1 always starts with 0.
//          * Check if the first bit of R1 is 0 and stop waiting
//          */
//         if (!(*dst & R1_SPI_ERROR)) {
//             break;
//         }
//     }
//     // read the rest of the response
//     if (spi_mmc_resp_size(resp_type) > 1) {
//         tmp = *dst;
//         free(dst);
//         dst = (uint8_t *)calloc(1, spi_mmc_resp_size(resp_type));
//         spi_read_blocking(spi, 0xFF, &dst[1],
//                             spi_mmc_resp_size(resp_type) - 1);
//         memcpy(dst, &tmp, 1);
//     }

//     printf("\n\n");

//     if (resp && spi_mmc_parse_response(dst, resp, resp_type)) {
//         printf("Error parsing SPI response: %d\n", ret);
//         free(dst);
//         return -EIO;
//     }
//     free(dst);
//     cs_deselect(cs_pin);
//     // if (ret != spi_mmc_resp_size(resp_type)) {
//     //     printf("Error sending SPI message. Wanted bits: %d; got: %d\n",
//     //             spi_mmc_resp_size(resp_type), ret);
//     //     return -EIO;
//     // }
//     return 0;
// }

static int send_msg(struct spi_mmc_dev_data *data,
                    const struct spi_mmc_message msg, uint8_t* resp)
{
    /**
     * @note: no nullptr error handling done here for better performance.
     * Make sure values are valid before calling send_msg!
     */
    uint8_t ret;

    cs_select(data->cs_pin);
    ret = spi_send_cmd(data, msg, resp);
    cs_deselect(data->cs_pin);

    return 0;
}

static int send_reset(struct spi_mmc_dev_data *data)
{
    int ret = 0;
    int i;
    const struct spi_mmc_message reset_cmd = SPI_MMC_CMD_CRC(
                                                MMC_CMD_GO_IDLE_STATE, 0, 0x4A);

    uint8_t resp = 0xff;
    for (i = 0; i < 10; i++) {
        ret = send_msg(data, reset_cmd, &resp);
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
    } else if (resp & R1_SPI_IDLE) {
        printf("MMC SPI reset command succeeded.\n");
    } else {
        printf("MMC SPI reset command timed out.\n");
        ret = -ETIMEDOUT;
    }

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
    uint8_t response = 0xff;
    uint8_t *cmd8_resp;
    int ret;
    struct spi_mmc_dev_data *data = (struct spi_mmc_dev_data *)dev->priv;
    struct spi_mmc_message acmd41 = SPI_MMC_CMD(SD_CMD_APP_SEND_OP_COND, 0);
    const struct spi_mmc_message cmd8 = SPI_MMC_CMD_CRC(
                                            MMC_CMD_SEND_EXT_CSD, 0x1AA, 0x43);
    const struct spi_mmc_message cmd55 = SPI_MMC_CMD(MMC_CMD_APP_CMD, 0);
    const struct spi_mmc_message cmd58 = SPI_MMC_CMD(MMC_CMD_SPI_READ_OCR, 0);

    cmd8_resp = malloc(spi_mmc_resp_size(mmc_cmd_resp_type(cmd8.cmd)));
    ret = send_msg(data, cmd8, cmd8_resp);
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
        ret = send_msg(data, cmd55, &response);
        if (ret) {
            printf("Sending CMD55 failed.\n");
            return -EIO;
        }

        response = 0xff;
        ret = spi_mmc_wait_ready(data);
        if (ret) {
            printf("Waiting for card to be ready failed.\n");
            continue;
        }

        ret = send_msg(data, acmd41, &response);
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
        if (send_msg(data, cmd58, &response)) {
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

static int spi_mmc_init(struct mmc_dev *dev)
{
    struct spi_mmc_message spi_init_message = { 0 };

    int ret;
    uint i;
    uint8_t resp = 0xff;
    struct spi_mmc_dev_data *data = (struct spi_mmc_dev_data *)dev->priv;
    if (!data) {
        printf("No SPI MMC data available!\n");
        return -EINVAL;
    }

    spi_mmc_go_low_frequency(data);

    // Wait for at least 74 cycles with MOSI and CS asserted
    memset(&spi_init_message, 0xff, sizeof(spi_init_message));
    for (i = 0; i < (74 / (sizeof(spi_init_message) * 8) + 1); i++) {
        send_msg(data, spi_init_message, &resp);
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

static int aurora_spi_init(struct spi_drv *spi) {
    auto_init_mutex(aurora_spi_init_mutex);
    mutex_enter_blocking(&aurora_spi_init_mutex);

    if (!spi->initialized) {
        //// The SPI may be shared (using multiple SSs); protect it
        //spi->mutex = xSemaphoreCreateRecursiveMutex();
        //xSemaphoreTakeRecursive(spi->mutex, portMAX_DELAY);
        if (!mutex_is_initialized(&spi->mutex)) mutex_init(&spi->mutex);
        spi_lock(spi);

        // Default:
        if (!spi->baud_rate)
            spi->baud_rate = 10 * 1000 * 1000;
        // For the IRQ notification:
        sem_init(&spi->sem, 0, 1);

        /* Configure component */
        // Enable SPI at 100 kHz and connect to GPIOs
        spi_init(spi->hw_spi, 100 * 1000);
        spi_set_format(spi->hw_spi, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

        gpio_set_function(spi->miso_gpio, GPIO_FUNC_SPI);
        gpio_set_function(spi->mosi_gpio, GPIO_FUNC_SPI);
        gpio_set_function(spi->sck_gpio, GPIO_FUNC_SPI);

        // TODO: Figure this out
        // bi_decl(bi_3pins_with_func(spi->miso_gpio, spi->mosi_gpio, spi->sck_gpio, GPIO_FUNC_SPI));

        // Slew rate limiting levels for GPIO outputs.
        // enum gpio_slew_rate { GPIO_SLEW_RATE_SLOW = 0, GPIO_SLEW_RATE_FAST = 1 }
        // void gpio_set_slew_rate (uint gpio,enum gpio_slew_rate slew)
        // Default appears to be GPIO_SLEW_RATE_SLOW.

        // Drive strength levels for GPIO outputs.
        // enum gpio_drive_strength { GPIO_DRIVE_STRENGTH_2MA = 0, GPIO_DRIVE_STRENGTH_4MA = 1, GPIO_DRIVE_STRENGTH_8MA = 2,
        // GPIO_DRIVE_STRENGTH_12MA = 3 }
        // enum gpio_drive_strength gpio_get_drive_strength (uint gpio)
        if (spi->set_drive_strength) {
            gpio_set_drive_strength(spi->mosi_gpio, spi->mosi_gpio_drive_strength);
            gpio_set_drive_strength(spi->sck_gpio, spi->sck_gpio_drive_strength);
        }

        // SD cards' DO MUST be pulled up.
        gpio_pull_up(spi->miso_gpio);

        if (!spi->use_dma) {
            spi_unlock(spi);
            goto out;
        }

        // Grab some unused dma channels
        spi->tx_dma = dma_claim_unused_channel(true);
        spi->rx_dma = dma_claim_unused_channel(true);

        spi->tx_dma_cfg = dma_channel_get_default_config(spi->tx_dma);
        spi->rx_dma_cfg = dma_channel_get_default_config(spi->rx_dma);
        channel_config_set_transfer_data_size(&spi->tx_dma_cfg, DMA_SIZE_8);
        channel_config_set_transfer_data_size(&spi->rx_dma_cfg, DMA_SIZE_8);

        // We set the outbound DMA to transfer from a memory buffer to the SPI
        // transmit FIFO paced by the SPI TX FIFO DREQ The default is for the
        // read address to increment every element (in this case 1 byte -
        // DMA_SIZE_8) and for the write address to remain unchanged.
        channel_config_set_dreq(&spi->tx_dma_cfg, spi_get_index(spi->hw_spi)
                                                       ? DREQ_SPI1_TX
                                                       : DREQ_SPI0_TX);
        channel_config_set_write_increment(&spi->tx_dma_cfg, false);

        // We set the inbound DMA to transfer from the SPI receive FIFO to a
        // memory buffer paced by the SPI RX FIFO DREQ We coinfigure the read
        // address to remain unchanged for each element, but the write address
        // to increment (so data is written throughout the buffer)
        channel_config_set_dreq(&spi->rx_dma_cfg, spi_get_index(spi->hw_spi)
                                                       ? DREQ_SPI1_RX
                                                       : DREQ_SPI0_RX);
        channel_config_set_read_increment(&spi->rx_dma_cfg, false);

        /* Theory: we only need an interrupt on rx complete,
        since if rx is complete, tx must also be complete. */

        /* Configure the processor to run dma_handler() when DMA IRQ 0/1 is asserted */

        spi->DMA_IRQ_num = irqChannel1 ? DMA_IRQ_1 : DMA_IRQ_0;

        // Tell the DMA to raise IRQ line 0/1 when the channel finishes a block
        static void (*spi_irq_handler_p)();
        switch (spi->DMA_IRQ_num) {
        case DMA_IRQ_0:
            spi_irq_handler_p = spi_irq_handler_0;
            dma_channel_set_irq0_enabled(spi->rx_dma, true);
            dma_channel_set_irq0_enabled(spi->tx_dma, false);
        break;
        case DMA_IRQ_1:
            spi_irq_handler_p = spi_irq_handler_1;
            dma_channel_set_irq1_enabled(spi->rx_dma, true);
            dma_channel_set_irq1_enabled(spi->tx_dma, false);
        break;
        default:
            assert(false);
        }
        if (irqShared) {
            irq_add_shared_handler(
                spi->DMA_IRQ_num, *spi_irq_handler_p,
                PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
        } else {
            irq_set_exclusive_handler(spi->DMA_IRQ_num, *spi_irq_handler_p);
        }
        irq_set_enabled(spi->DMA_IRQ_num, true);
        spi_unlock(spi);
    }

out:
    spi->initialized = true;
    mutex_exit(&aurora_spi_init_mutex);
    return true;
}

struct mmc_drv *spi_mmc_drv_init(struct spi_drv *spi, uint cs_pin)
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

    struct spi_mmc_dev_data *data = (struct spi_mmc_dev_data *)
        calloc(1, sizeof(struct spi_mmc_dev_data));
    if (!data) {
        printf("Could not allocate SPI MMC data: %d\n", -ENOMEM);
        goto out;
    }
    data->spi = spi;
    data->cs_pin = cs_pin;

    // TODO: Figure this out
    // bi_decl(bi_1pin_with_name(cs_pin, "MMC SPI CS"));

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

    mutex_exit(&spi_mmc_drv_init_mutex);
    return drv;

free_ops:
    free(ops);
free_dev:
    free(dev);
free_data:
    free(data);
out:
    mutex_exit(&spi_mmc_drv_init_mutex);
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
