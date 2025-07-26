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

#include <aurora/config.h>
#include <aurora/crc.h>
#include <aurora/drivers/mmc/mmc.h>
#include <aurora/drivers/mmc/spi_mmc.h>
#include <aurora/drivers/spi.h>
#include <aurora/list.h>
#include <aurora/log.h>
#include <aurora/macros.h>

/* MMC/SD in SPI mode reports R1 status always */
#define R1_SPI_IDLE			BIT(0)
#define R1_SPI_ERASE_RESET		BIT(1)
#define R1_SPI_ILLEGAL_COMMAND		BIT(2)
#define R1_SPI_COM_CRC			BIT(3)
#define R1_SPI_ERASE_SEQ		BIT(4)
#define R1_SPI_ADDRESS			BIT(5)
#define R1_SPI_PARAMETER		BIT(6)
/* R1 bit 7 is always zero, reuse this bit for error */
#define R1_SPI_ERROR			BIT(7)

/* Response tokens used to ack each block written: */
#define SPI_MMC_RESPONSE_CODE(x)	((x) & 0x1f)
#define SPI_RESPONSE_ACCEPTED		((2 << 1)|1)
#define SPI_RESPONSE_CRC_ERR		((5 << 1)|1)
#define SPI_RESPONSE_WRITE_ERR		((6 << 1)|1)

/*
 * Read and write blocks start with these tokens and end with crc;
 * on error, read tokens act like a subset of R2_SPI_* values.
 */
/* single block write multiblock read */
#define SPI_TOKEN_SINGLE		0xfe
/* multiblock write */
#define SPI_TOKEN_MULTI_WRITE		0xfc
/* terminate multiblock write */
#define SPI_TOKEN_STOP_TRAN		0xfd

/* MMC SPI commands start with a start bit "0" and a transmit bit "1" */
#define MMC_SPI_CMD(x) (0x40 | (x))

/* bus capability */
#define MMC_SPI_VOLTAGE			(MMC_VDD_32_33 | MMC_VDD_33_34)
#define MMC_SPI_MIN_CLOCK		400000	/* 400KHz to meet MMC spec */
#define MMC_SPI_MAX_CLOCK		25000000 /* SD/MMC legacy speed */

/* timeout value */
#define CMD_TIMEOUT             8
#define READ_TIMEOUT            3000000 /* 1 sec */
#define WRITE_TIMEOUT           3000000 /* 1 sec */
#define R1B_TIMEOUT             3000000 /* 1 sec */

static int spi_mmc_transfer(struct spi_mmc_context *ctx, const uint8_t *tx,
                            uint8_t* rx, size_t length)
{
    int len;
    int ret = 0;
    const uint8_t tx_dummy = 0xff;
    uint8_t rx_dummy = 0;

    // assert(512 == length || 1 == length);
    assert(tx || rx);
    // assert(!(tx && rx));

    cs_select(ctx->cs_pin);

    tx = tx ? tx : &tx_dummy;
    rx = rx ? rx : &rx_dummy;

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

/**
 * mmc_spi_sendcmd() - send a command to the SD card
 *
 * @ctx:	mmc_spi device
 * @cmdidx:	command index
 * @cmdarg:	command argument
 * @resp_type:	card response type
 * @resp:	buffer to store the card response
 * @resp_size:	size of the card response
 * @resp_match:	if true, compare each of received bytes with @resp_match_value
 * @resp_match_value:	a value to be compared with each of received bytes
 * @r1b:	if true, receive additional bytes for busy signal token
 * Return: 0 if OK, -ETIMEDOUT if no card response is received, -ve on error
 */
static int mmc_spi_sendcmd(struct spi_mmc_context *ctx,
			   uint8_t cmdidx, uint32_t cmdarg, uint32_t resp_type,
			   uint8_t *resp, uint32_t resp_size,
			   bool resp_match, uint8_t resp_match_value, bool r1b)
{
	int i, rpos = 0, ret = 0;
	uint8_t cmdo[7], r;

	if (!resp || !resp_size)
		return 0;

	log_trace("cmd%d cmdarg=0x%x resp_type=0x%x "
	      "resp_size=%d resp_match=%d resp_match_value=0x%x\n",
	      cmdidx, cmdarg, resp_type,
	      resp_size, resp_match, resp_match_value);

	cmdo[0] = 0xff;
	cmdo[1] = MMC_SPI_CMD(cmdidx);
	cmdo[2] = cmdarg >> 24;
	cmdo[3] = cmdarg >> 16;
	cmdo[4] = cmdarg >> 8;
	cmdo[5] = cmdarg;
	cmdo[6] = (crc7(&cmdo[1], 5) << 1) | 0x01;

	ret = spi_mmc_transfer(ctx, cmdo, NULL, sizeof(cmdo) * 8);
	if (ret)
		return ret;

	ret = spi_mmc_transfer(ctx, NULL, &r, 1 * 8);
	if (ret)
		return ret;

	log_debug("%s: cmd%d", __func__, cmdidx);

	if (resp_match)
		r = ~resp_match_value;
	i = CMD_TIMEOUT;
	while (i) {
		ret = spi_mmc_transfer(ctx, NULL, &r, 1 * 8);
		if (ret)
			return ret;
		log_debug(" resp%d=0x%x", rpos, r);
		rpos++;
		i--;

		if (resp_match) {
			if (r == resp_match_value)
				break;
		} else {
			if (!(r & 0x80))
				break;
		}

		if (!i)
			return -ETIMEDOUT;
	}

	resp[0] = r;
	for (i = 1; i < resp_size; i++) {
		ret = spi_mmc_transfer(ctx, NULL, &r, 1 * 8);
		if (ret)
			return ret;
		log_debug(" resp%d=0x%x", rpos, r);
		rpos++;
		resp[i] = r;
	}

	if (r1b == true) {
		i = R1B_TIMEOUT;
		while (i) {
			ret = spi_mmc_transfer(ctx, NULL, &r, 1 * 8);
			if (ret)
				return ret;

			log_debug(" resp%d=0x%x", rpos, r);
			rpos++;
			i--;

			if (r)
				break;
		}
		if (!i)
			return -ETIMEDOUT;
	}

	return 0;
}

/**
 * mmc_spi_readdata() - read data block(s) from the SD card
 *
 * @ctx:	mmc_spi device context
 * @xbuf:	buffer of the actual data (excluding token and crc) to read
 * @bcnt:	number of data blocks to transfer
 * @bsize:	size of the actual data (excluding token and crc) in bytes
 * Return: 0 if OK, -ECOMM if crc error, -ETIMEDOUT on other errors
 */
static int mmc_spi_readdata(struct spi_mmc_context *ctx,
			    void *xbuf, uint32_t bcnt, uint32_t bsize)
{
	uint8_t crc;
	uint8_t *buf = xbuf, r1;
	int i, ret = 0;

	while (bcnt--) {
		for (i = 0; i < READ_TIMEOUT; i++) {
			ret = spi_mmc_transfer(ctx, NULL, &r1, 1 * 8);
			if (ret)
				return ret;
			if (r1 == SPI_TOKEN_SINGLE)
				break;
		}
		log_debug("%s: data tok%d 0x%x\n", __func__, i, r1);
		if (r1 == SPI_TOKEN_SINGLE) {
			ret = spi_mmc_transfer(ctx, NULL, buf, bsize * 8);
			if (ret)
				return ret;
			ret = spi_mmc_transfer(ctx, NULL, &crc, 2 * 8);
			if (ret)
				return ret;
#if IS_ENABLED(CONFIG_MMC_SPI_CRC_ON)
			uint8_t crc_ok = crc16_ccitt(buf, bsize);
			if (crc_ok != crc) {
				log_debug("%s: data crc error, expected %02x got %02x\n",
				      __func__, crc_ok, crc);
				r1 = R1_SPI_COM_CRC;
				break;
			}
#endif
			r1 = 0;
		} else {
			r1 = R1_SPI_ERROR;
			break;
		}
		buf += bsize;
	}

	if (r1 & R1_SPI_COM_CRC)
		ret = -EBADMSG;
	else if (r1) /* other errors */
		ret = -ETIMEDOUT;

	return ret;
}


/**
 * mmc_spi_writedata() - write data block(s) to the SD card
 *
 * @ctx:	mmc_spi device context
 * @xbuf:	buffer of the actual data (excluding token and crc) to write
 * @bcnt:	number of data blocks to transfer
 * @bsize:	size of actual data (excluding token and crc) in bytes
 * @multi:	indicate a transfer by multiple block write command (CMD25)
 * Return: 0 if OK, -ECOMM if crc error, -ETIMEDOUT on other errors
 */
static int mmc_spi_writedata(struct spi_mmc_context *ctx, const void *xbuf,
			     uint32_t bcnt, uint32_t bsize, int multi)
{
	const uint8_t *buf = xbuf;
	uint8_t r1, tok[2];
	uint8_t crc;
	int i, ret = 0;

	tok[0] = 0xff;
	tok[1] = multi ? SPI_TOKEN_MULTI_WRITE : SPI_TOKEN_SINGLE;

	while (bcnt--) {
#if IS_ENABLED(CONFIG_MMC_SPI_CRC_ON)
		crc = crc7((u8 *)buf, bsize);
#endif
		spi_mmc_transfer(ctx, tok, NULL, 2 * 8);
		spi_mmc_transfer(ctx, buf, NULL, bsize * 8);
		spi_mmc_transfer(ctx, &crc, NULL, 2 * 8);
		for (i = 0; i < CMD_TIMEOUT; i++) {
			spi_mmc_transfer(ctx, NULL, &r1, 1 * 8);
			if ((r1 & 0x10) == 0) /* response token */
				break;
		}
		log_debug("%s: data tok%d 0x%x\n", __func__, i, r1);
		if (SPI_MMC_RESPONSE_CODE(r1) == SPI_RESPONSE_ACCEPTED) {
			log_debug("%s: data accepted\n", __func__);
			for (i = 0; i < WRITE_TIMEOUT; i++) { /* wait busy */
				spi_mmc_transfer(ctx, NULL, &r1, 1 * 8);
				if (i && r1 == 0xff) {
					r1 = 0;
					break;
				}
			}
			if (i == WRITE_TIMEOUT) {
				log_debug("%s: data write timeout 0x%x\n",
				      __func__, r1);
				r1 = R1_SPI_ERROR;
				break;
			}
		} else {
			log_debug("%s: data error 0x%x\n", __func__, r1);
			r1 = R1_SPI_COM_CRC;
			break;
		}
		buf += bsize;
	}
	if (multi && bcnt == -1) { /* stop multi write */
		tok[1] = SPI_TOKEN_STOP_TRAN;
		spi_mmc_transfer(ctx, tok, NULL, 2 * 8);
		for (i = 0; i < WRITE_TIMEOUT; i++) { /* wait busy */
			spi_mmc_transfer(ctx, NULL, &r1, 1 * 8);
			if (i && r1 == 0xff) {
				r1 = 0;
				break;
			}
		}
		if (i == WRITE_TIMEOUT) {
			log_debug("%s: data write timeout 0x%x\n", __func__, r1);
			r1 = R1_SPI_ERROR;
		}
	}

	if (r1 & R1_SPI_COM_CRC)
		ret = -EBADMSG;
	else if (r1) /* other errors */
		ret = -ETIMEDOUT;

	return ret;
}

/**
 * @brief mmc_spi_request - Send a command to the card via SPI
 * 
 * @param dev: mmc device to send command to
 * @param cmd: Command to send
 * @param data: Additional data to send/receive
 * @r1b:	if true, receive additional bytes for busy signal token
 * Return: 0 if OK, -ETIMEDOUT if no card response is received, -ve on error
 */
static int mmc_spi_request(struct mmc_dev *dev, struct mmc_cmd *cmd,
						   struct mmc_data *data)
{
	int i, multi, ret = 0;
	uint8_t *resp = NULL;
	uint32_t resp_size = 0;
	bool resp_match = false, r1b = false;
	uint8_t resp8 = 0, resp16[2] = { 0 }, resp40[5] = { 0 }, resp_match_value = 0;
	struct spi_mmc_context *ctx = (struct spi_mmc_context *)dev->priv;

	spi_lock(ctx->spi);

	for (i = 0; i < 4; i++)
		cmd->response[i] = 0;

	switch (cmd->cmdidx) {
	case SD_CMD_APP_SEND_OP_COND:
	case MMC_CMD_SEND_OP_COND:
		resp = &resp8;
		resp_size = sizeof(resp8);
		cmd->cmdarg = 0x40000000;
		break;
	case SD_CMD_SEND_IF_COND:
		resp = (uint8_t *)&resp40[0];
		resp_size = sizeof(resp40);
		resp_match = true;
		resp_match_value = R1_SPI_IDLE;
		break;
	case MMC_CMD_SPI_READ_OCR:
		resp = (uint8_t *)&resp40[0];
		resp_size = sizeof(resp40);
		break;
	case MMC_CMD_SEND_STATUS:
		resp = (uint8_t *)&resp16[0];
		resp_size = sizeof(resp16);
		break;
	case MMC_CMD_SET_BLOCKLEN:
	case MMC_CMD_SPI_CRC_ON_OFF:
		resp = &resp8;
		resp_size = sizeof(resp8);
		resp_match = true;
		resp_match_value = 0x0;
		break;
	case MMC_CMD_STOP_TRANSMISSION:
	case MMC_CMD_ERASE:
		resp = &resp8;
		resp_size = sizeof(resp8);
		r1b = true;
		break;
	case MMC_CMD_SEND_CSD:
	case MMC_CMD_SEND_CID:
	case MMC_CMD_READ_SINGLE_BLOCK:
	case MMC_CMD_READ_MULTIPLE_BLOCK:
	case MMC_CMD_WRITE_SINGLE_BLOCK:
	case MMC_CMD_WRITE_MULTIPLE_BLOCK:
	case MMC_CMD_APP_CMD:
	case SD_CMD_ERASE_WR_BLK_START:
	case SD_CMD_ERASE_WR_BLK_END:
		resp = &resp8;
		resp_size = sizeof(resp8);
		break;
	default:
		resp = &resp8;
		resp_size = sizeof(resp8);
		resp_match = true;
		resp_match_value = R1_SPI_IDLE;
		break;
	};

	ret = mmc_spi_sendcmd(dev->priv, cmd->cmdidx, cmd->cmdarg, cmd->resp_type,
			      resp, resp_size, resp_match, resp_match_value, r1b);
	if (ret)
		goto done;

	switch (cmd->cmdidx) {
	case SD_CMD_APP_SEND_OP_COND:
	case MMC_CMD_SEND_OP_COND:
		cmd->response[0] = (resp8 & R1_SPI_IDLE) ? 0 : OCR_BUSY;
		break;
	case SD_CMD_SEND_IF_COND:
	case MMC_CMD_SPI_READ_OCR:
		cmd->response[0] = resp40[4];
		cmd->response[0] |= (uint)resp40[3] << 8;
		cmd->response[0] |= (uint)resp40[2] << 16;
		cmd->response[0] |= (uint)resp40[1] << 24;
		break;
	case MMC_CMD_SEND_STATUS:
		if (resp16[0] || resp16[1])
			cmd->response[0] = MMC_STATUS_ERROR;
		else
			cmd->response[0] = MMC_STATUS_RDY_FOR_DATA;
		break;
	case MMC_CMD_SEND_CID:
	case MMC_CMD_SEND_CSD:
		ret = mmc_spi_readdata(dev->priv, cmd->response, 1, 16);
		if (ret)
			return ret;
		for (i = 0; i < 4; i++)
			cmd->response[i] =
				cpu_to_be32(cmd->response[i]);
		break;
	default:
		cmd->response[0] = resp8;
		break;
	}

	log_debug("%s: cmd%d resp0=0x%x resp1=0x%x resp2=0x%x resp3=0x%x\n",
	      __func__, cmd->cmdidx, cmd->response[0], cmd->response[1],
	      cmd->response[2], cmd->response[3]);

	if (data) {
		log_debug("%s: data flags=0x%x blocks=%d block_size=%d\n",
		      __func__, data->flags, data->blocks, data->blocksize);
		multi = (cmd->cmdidx == MMC_CMD_WRITE_MULTIPLE_BLOCK);
		if (data->flags == MMC_DATA_READ)
			ret = mmc_spi_readdata(dev->priv, data->dest,
					       data->blocks, data->blocksize);
		else if  (data->flags == MMC_DATA_WRITE)
			ret = mmc_spi_writedata(dev->priv, data->src,
						data->blocks, data->blocksize,
						multi);
	}

done:
	spi_unlock(ctx->spi);

	return ret;
}

static int mmc_spi_send_cmd(struct spi_mmc_context *ctx, struct mmc_cmd *cmd,
			      struct mmc_data *data)
{
	int i, multi, ret = 0;
	uint8_t *resp = NULL;
	uint32_t resp_size = 0;
	bool resp_match = false, r1b = false;
	uint8_t resp8 = 0, resp16[2] = { 0 }, resp40[5] = { 0 }, resp_match_value = 0;

	for (i = 0; i < 4; i++)
		cmd->response[i] = 0;

	switch (cmd->cmdidx) {
	case SD_CMD_APP_SEND_OP_COND:
	case MMC_CMD_SEND_OP_COND:
		resp = &resp8;
		resp_size = sizeof(resp8);
		cmd->cmdarg = 0x40000000;
		break;
	case SD_CMD_SEND_IF_COND:
		resp = (uint8_t *)&resp40[0];
		resp_size = sizeof(resp40);
		resp_match = true;
		resp_match_value = R1_SPI_IDLE;
		break;
	case MMC_CMD_SPI_READ_OCR:
		resp = (uint8_t *)&resp40[0];
		resp_size = sizeof(resp40);
		break;
	case MMC_CMD_SEND_STATUS:
		resp = (uint8_t *)&resp16[0];
		resp_size = sizeof(resp16);
		break;
	case MMC_CMD_SET_BLOCKLEN:
	case MMC_CMD_SPI_CRC_ON_OFF:
		resp = &resp8;
		resp_size = sizeof(resp8);
		resp_match = true;
		resp_match_value = 0x0;
		break;
	case MMC_CMD_STOP_TRANSMISSION:
	case MMC_CMD_ERASE:
		resp = &resp8;
		resp_size = sizeof(resp8);
		r1b = true;
		break;
	case MMC_CMD_SEND_CSD:
	case MMC_CMD_SEND_CID:
	case MMC_CMD_READ_SINGLE_BLOCK:
	case MMC_CMD_READ_MULTIPLE_BLOCK:
	case MMC_CMD_WRITE_SINGLE_BLOCK:
	case MMC_CMD_WRITE_MULTIPLE_BLOCK:
	case MMC_CMD_APP_CMD:
	case SD_CMD_ERASE_WR_BLK_START:
	case SD_CMD_ERASE_WR_BLK_END:
		resp = &resp8;
		resp_size = sizeof(resp8);
		break;
	default:
		resp = &resp8;
		resp_size = sizeof(resp8);
		resp_match = true;
		resp_match_value = R1_SPI_IDLE;
		break;
	};

	ret = mmc_spi_sendcmd(ctx, cmd->cmdidx, cmd->cmdarg, cmd->resp_type,
			      resp, resp_size, resp_match, resp_match_value, r1b);
	if (ret)
		goto done;

	switch (cmd->cmdidx) {
	case SD_CMD_APP_SEND_OP_COND:
	case MMC_CMD_SEND_OP_COND:
		cmd->response[0] = (resp8 & R1_SPI_IDLE) ? 0 : OCR_BUSY;
		break;
	case SD_CMD_SEND_IF_COND:
	case MMC_CMD_SPI_READ_OCR:
		cmd->response[0] = resp40[4];
		cmd->response[0] |= (uint)resp40[3] << 8;
		cmd->response[0] |= (uint)resp40[2] << 16;
		cmd->response[0] |= (uint)resp40[1] << 24;
		break;
	case MMC_CMD_SEND_STATUS:
		if (resp16[0] || resp16[1])
			cmd->response[0] = MMC_STATUS_ERROR;
		else
			cmd->response[0] = MMC_STATUS_RDY_FOR_DATA;
		break;
	case MMC_CMD_SEND_CID:
	case MMC_CMD_SEND_CSD:
		ret = mmc_spi_readdata(ctx, cmd->response, 1, 16);
		if (ret)
			return ret;
		for (i = 0; i < 4; i++)
			cmd->response[i] =
				cpu_to_be32(cmd->response[i]);
		break;
	default:
		cmd->response[0] = resp8;
		break;
	}

	log_debug("%s: cmd%d resp0=0x%x resp1=0x%x resp2=0x%x resp3=0x%x\n",
	      __func__, cmd->cmdidx, cmd->response[0], cmd->response[1],
	      cmd->response[2], cmd->response[3]);

	if (data) {
		log_debug("%s: data flags=0x%x blocks=%d block_size=%d\n",
		      __func__, data->flags, data->blocks, data->blocksize);
		multi = (cmd->cmdidx == MMC_CMD_WRITE_MULTIPLE_BLOCK);
		if (data->flags == MMC_DATA_READ)
			ret = mmc_spi_readdata(ctx, data->dest,
					       data->blocks, data->blocksize);
		else if  (data->flags == MMC_DATA_WRITE)
			ret = mmc_spi_writedata(ctx, data->src,
						data->blocks, data->blocksize,
						multi);
	}

done:
	spi_mmc_transfer(ctx, NULL, NULL, 0);

	return ret;
}

/*----------------------------------------------------------------------------*/

static int mmc_spi_set_ios(struct mmc_dev *dev)
{
	return 0;
}

/*----------------------------------------------------------------------------*/

static struct mmc_ops drv_ops = {
	.set_ios	= mmc_spi_set_ios,
	.send_cmd = mmc_spi_request,
};

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