/**
 * @file mmc.c
 * @brief Sources for generic MMC/SD I/O
 * @note This file contains the source code for generic MMC/SD I/O.
 *
 * Author: Maximilian Stephan @ Auxspace e.V.
 * Copyright (C) 2025 Auxspace e.V.
 */
#include "pico/stdlib.h"
#include <errno.h>

#include <aurora/drivers/mmc/mmc.h>
#include <aurora/log.h>
#include <linux/sizes.h>

#define DEFAULT_CMD6_TIMEOUT_MS  500

/*
 * TRAN_SPEED bits 0:2 encode the frequency unit:
 * 0 = 100KHz, 1 = 1MHz, 2 = 10MHz, 3 = 100MHz, values 4 - 7 are reserved.
 * The values in fbase[] are divided by 10 to avoid floats in multiplier[].
 */
static const int fbase[] = {
	10000,
	100000,
	1000000,
	10000000,
	0,	/* reserved */
	0,	/* reserved */
	0,	/* reserved */
	0,	/* reserved */
};

/* Multiplier values for TRAN_SPEED.  Multiplied by 10 to be nice
 * to platforms without floating point.
 */
static const uint8_t multipliers[] = {
	0,	/* reserved */
	10,
	12,
	13,
	15,
	20,
	25,
	30,
	35,
	40,
	45,
	50,
	55,
	60,
	70,
	80,
};

static const struct mode_width_tuning mmc_modes_by_pref[] = {
#if IS_ENABLED(CONFIG_MMC_HS400_ES_SUPPORT)
	{
		.mode = MMC_HS_400_ES,
		.widths = MMC_MODE_8BIT,
	},
#endif
#if IS_ENABLED(CONFIG_MMC_HS400_SUPPORT)
	{
		.mode = MMC_HS_400,
		.widths = MMC_MODE_8BIT,
		.tuning = MMC_CMD_SEND_TUNING_BLOCK_HS200
	},
#endif
#if IS_ENABLED(CONFIG_MMC_HS200_SUPPORT)
	{
		.mode = MMC_HS_200,
		.widths = MMC_MODE_8BIT | MMC_MODE_4BIT,
		.tuning = MMC_CMD_SEND_TUNING_BLOCK_HS200
	},
#endif
	{
		.mode = MMC_DDR_52,
		.widths = MMC_MODE_8BIT | MMC_MODE_4BIT,
	},
	{
		.mode = MMC_HS_52,
		.widths = MMC_MODE_8BIT | MMC_MODE_4BIT | MMC_MODE_1BIT,
	},
	{
		.mode = MMC_HS,
		.widths = MMC_MODE_8BIT | MMC_MODE_4BIT | MMC_MODE_1BIT,
	},
	{
		.mode = MMC_LEGACY,
		.widths = MMC_MODE_8BIT | MMC_MODE_4BIT | MMC_MODE_1BIT,
	}
};

#define for_each_mmc_mode_by_pref(caps, mwt) \
	for (mwt = mmc_modes_by_pref;\
	    mwt < mmc_modes_by_pref + ARRAY_SIZE(mmc_modes_by_pref);\
	    mwt++) \
		if (caps & MMC_CAP(mwt->mode))

static const struct ext_csd_bus_width {
	uint32_t cap;
	bool is_ddr;
	uint32_t ext_csd_bits;
} ext_csd_bus_width[] = {
	{MMC_MODE_8BIT, true, EXT_CSD_DDR_BUS_WIDTH_8},
	{MMC_MODE_4BIT, true, EXT_CSD_DDR_BUS_WIDTH_4},
	{MMC_MODE_8BIT, false, EXT_CSD_BUS_WIDTH_8},
	{MMC_MODE_4BIT, false, EXT_CSD_BUS_WIDTH_4},
	{MMC_MODE_1BIT, false, EXT_CSD_BUS_WIDTH_1},
};

struct mode_width_tuning {
	enum bus_mode mode;
	uint32_t widths;
	uint32_t tuning;
};

int mmc_send_cmd(struct mmc_drv *mmc, struct mmc_cmd *cmd, struct mmc_data *data)
{
	const struct mmc_ops *ops = mmc->ops;
	struct mmc_dev *dev = mmc->dev;
	int ret;

	log_trace("mmc before send");
	if (ops->send_cmd)
		ret = ops->send_cmd(dev, cmd, data);
	else
		ret = -ENOSYS;
	log_trace("mmc after send");

	return ret;
}

/**
 * mmc_send_cmd_retry() - send a command to the mmc device, retrying on error
 *
 * @dev:	device to receive the command
 * @cmd:	command to send
 * @data:	additional data to send/receive
 * @retries:	how many times to retry; mmc_send_cmd is always called at least
 *              once
 * Return: 0 if ok, -ve on error
 */
static int mmc_send_cmd_retry(struct mmc_drv *mmc, struct mmc_cmd *cmd,
			      struct mmc_data *data, uint retries)
{
	int ret;

	do {
		ret = mmc_send_cmd(mmc, cmd, data);
	} while (ret && retries--);

	return ret;
}

static int mmc_wait_dat0(struct mmc_drv *mmc, int state, int timeout_us)
{
	if (mmc->ops->wait_dat0)
		return mmc->ops->wait_dat0(mmc->dev, state, timeout_us);

	return -ENOSYS;
}

const char *mmc_mode_name(enum bus_mode mode)
{
	static const char *const names[] = {
	      [MMC_LEGACY]	= "MMC legacy",
	      [MMC_HS]		= "MMC High Speed (26MHz)",
	      [SD_HS]		= "SD High Speed (50MHz)",
	      [UHS_SDR12]	= "UHS SDR12 (25MHz)",
	      [UHS_SDR25]	= "UHS SDR25 (50MHz)",
	      [UHS_SDR50]	= "UHS SDR50 (100MHz)",
	      [UHS_SDR104]	= "UHS SDR104 (208MHz)",
	      [UHS_DDR50]	= "UHS DDR50 (50MHz)",
	      [MMC_HS_52]	= "MMC High Speed (52MHz)",
	      [MMC_DDR_52]	= "MMC DDR52 (52MHz)",
	      [MMC_HS_200]	= "HS200 (200MHz)",
	      [MMC_HS_400]	= "HS400 (200MHz)",
	      [MMC_HS_400_ES]	= "HS400ES (200MHz)",
	};

	if (mode >= MMC_MODES_END)
		return "Unknown mode";
	else
		return names[mode];
}

static const struct mode_width_tuning sd_modes_by_pref[] = {
#if IS_ENABLED(CONFIG_MMC_UHS_SUPPORT)
#if IS_ENABLED(CONFIG_MMC_SUPPORTS_TUNING)
	{
		.mode = UHS_SDR104,
		.widths = MMC_MODE_4BIT | MMC_MODE_1BIT,
		.tuning = MMC_CMD_SEND_TUNING_BLOCK
	},
#endif
	{
		.mode = UHS_SDR50,
		.widths = MMC_MODE_4BIT | MMC_MODE_1BIT,
	},
	{
		.mode = UHS_DDR50,
		.widths = MMC_MODE_4BIT | MMC_MODE_1BIT,
	},
	{
		.mode = UHS_SDR25,
		.widths = MMC_MODE_4BIT | MMC_MODE_1BIT,
	},
#endif
	{
		.mode = SD_HS,
		.widths = MMC_MODE_4BIT | MMC_MODE_1BIT,
	},
#if IS_ENABLED(CONFIG_MMC_UHS_SUPPORT)
	{
		.mode = UHS_SDR12,
		.widths = MMC_MODE_4BIT | MMC_MODE_1BIT,
	},
#endif
	{
		.mode = MMC_LEGACY,
		.widths = MMC_MODE_4BIT | MMC_MODE_1BIT,
	}
};

#define for_each_sd_mode_by_pref(caps, mwt) \
	for (mwt = sd_modes_by_pref;\
	     mwt < sd_modes_by_pref + ARRAY_SIZE(sd_modes_by_pref);\
	     mwt++) \
		if (caps & MMC_CAP(mwt->mode))

static uint mmc_mode2freq(struct mmc_drv *mmc, enum bus_mode mode)
{
	static const int freqs[] = {
	      [MMC_LEGACY]	= 25000000,
	      [MMC_HS]		= 26000000,
	      [SD_HS]		= 50000000,
	      [MMC_HS_52]	= 52000000,
	      [MMC_DDR_52]	= 52000000,
	      [UHS_SDR12]	= 25000000,
	      [UHS_SDR25]	= 50000000,
	      [UHS_SDR50]	= 100000000,
	      [UHS_DDR50]	= 50000000,
	      [UHS_SDR104]	= 208000000,
	      [MMC_HS_200]	= 200000000,
	      [MMC_HS_400]	= 200000000,
	      [MMC_HS_400_ES]	= 200000000,
	};

	if (mode == MMC_LEGACY)
		return mmc->dev->legacy_speed;
	else if (mode >= MMC_MODES_END)
		return 0;
	else
		return freqs[mode];
}

/**
 * mmc_send_cmd_quirks() - send a command to the mmc device, retrying if a
 *                         specific quirk is enabled
 *
 * @dev:	device to receive the command
 * @cmd:	command to send
 * @data:	additional data to send/receive
 * @quirk:	retry only if this quirk is enabled
 * @retries:	how many times to retry; mmc_send_cmd is always called at least
 *              once
 * Return: 0 if ok, -ve on error
 */
static int mmc_send_cmd_quirks(struct mmc_drv *mmc, struct mmc_cmd *cmd,
			       struct mmc_data *data, uint32_t quirk, uint retries)
{
	if (IS_ENABLED(CONFIG_MMC_QUIRKS) && mmc->dev->quirks & quirk)
		return mmc_send_cmd_retry(mmc, cmd, data, retries);
	else
		return mmc_send_cmd(mmc, cmd, data);
}

static int mmc_set_ios(struct mmc_drv *mmc)
{
	struct mmc_ops *ops = mmc->ops;

	if (!ops->set_ios)
		return -ENOSYS;
	return ops->set_ios(mmc->dev);
}

int mmc_set_clock(struct mmc_drv *mmc, uint clock, bool disable)
{
	if (!disable) {
		if (clock > mmc->dev->f_max)
			clock = mmc->dev->f_max;

		if (clock < mmc->dev->f_min)
			clock = mmc->dev->f_min;
	}

	mmc->dev->clock = clock;
	mmc->dev->clk_disable = disable;

	log_debug("clock is %s (%dHz)\n", disable ? "disabled" : "enabled", clock);

	return mmc_set_ios(mmc);
}

static int mmc_go_idle(struct mmc_drv *mmc)
{
	struct mmc_cmd cmd;
	int err;

	sleep_ms(1);

	cmd.cmdidx = MMC_CMD_GO_IDLE_STATE;
	cmd.cmdarg = 0;
	cmd.resp_type = MMC_RSP_NONE;

	err = mmc_send_cmd(mmc, &cmd, NULL);

	if (err)
		return err;

	sleep_ms(2);

	return 0;
}

#if IS_ENABLED(CONFIG_MMC_SUPPORTS_TUNING)
static int mmc_execute_tuning(struct mmc_drv *mmc, uint32_t opcode)
{
	struct mmc_ops *ops = mmc->ops;
    int ret;

	mmc->tuning = true;
	if (!ops->execute_tuning)
		return -ENOSYS;

    ret = ops->execute_tuning(mmc->dev, opcode);
	mmc->tuning = false;

    return ret;
}
#endif

static int mmc_select_mode(struct mmc_drv *mmc, enum bus_mode mode)
{
	mmc->dev->selected_mode = mode;
	mmc->dev->tran_speed = mmc_mode2freq(mmc, mode);
	mmc->dev->ddr_mode = mmc_is_mode_ddr(mode);
	log_debug("selecting mode %s (freq : %d MHz)\n", mmc_mode_name(mode),
		 mmc->dev->tran_speed / 1000000);
	return 0;
}

int mmc_send_status(struct mmc_drv *mmc, unsigned int *status)
{
	struct mmc_cmd cmd;
	int ret;

	cmd.cmdidx = MMC_CMD_SEND_STATUS;
	cmd.resp_type = MMC_RSP_R1;
	if (!mmc_host_is_spi(mmc))
		cmd.cmdarg = mmc->dev->rca << 16;

	ret = mmc_send_cmd_retry(mmc, &cmd, NULL, 4);
	if (!ret)
		*status = cmd.response[0];

	return ret;
}

static int sd_send_op_cond(struct mmc_drv *mmc, bool uhs_en)
{
	const int timeout_ms = 1000;
    absolute_time_t timeout_time = make_timeout_time_ms(timeout_ms);
	int err;
	struct mmc_cmd cmd;

	while (1) {
		cmd.cmdidx = MMC_CMD_APP_CMD;
		cmd.resp_type = MMC_RSP_R1;
		cmd.cmdarg = 0;

		err = mmc_send_cmd(mmc, &cmd, NULL);

		if (err)
			return err;

		cmd.cmdidx = SD_CMD_APP_SEND_OP_COND;
		cmd.resp_type = MMC_RSP_R3;

		/*
		 * Most cards do not answer if some reserved bits
		 * in the ocr are set. However, Some controller
		 * can set bit 7 (reserved for low voltages), but
		 * how to manage low voltages SD card is not yet
		 * specified.
		 */
		cmd.cmdarg = mmc_host_is_spi(mmc) ? 0 :
			(mmc->dev->voltages & 0xff8000);

		if (mmc->dev->version == SD_VERSION_2)
			cmd.cmdarg |= OCR_HCS;

		if (uhs_en)
			cmd.cmdarg |= OCR_S18R;

		err = mmc_send_cmd(mmc, &cmd, NULL);

		if (err)
			return err;

		if (cmd.response[0] & OCR_BUSY)
			break;

		if (0 < absolute_time_diff_us(get_absolute_time(), timeout_time))
			return -EOPNOTSUPP;

		sleep_ms(1);
	}

	if (mmc->dev->version != SD_VERSION_2)
		mmc->dev->version = SD_VERSION_1_0;

	if (mmc_host_is_spi(mmc)) { /* read OCR for spi */
		cmd.cmdidx = MMC_CMD_SPI_READ_OCR;
		cmd.resp_type = MMC_RSP_R3;
		cmd.cmdarg = 0;

		err = mmc_send_cmd(mmc, &cmd, NULL);

		if (err)
			return err;
	}

	mmc->dev->ocr = cmd.response[0];

#if IS_ENABLED(CONFIG_MMC_UHS_SUPPORT)
	if (uhs_en && !(mmc_host_is_spi(mmc)) && (cmd.response[0] & 0x41000000)
	    == 0x41000000) {
		err = mmc_switch_voltage(mmc, MMC_SIGNAL_VOLTAGE_180);
		if (err)
			return err;
	}
#endif

	mmc->dev->high_capacity = ((mmc->dev->ocr & OCR_HCS) == OCR_HCS);
	mmc->dev->rca = 0;

	return 0;
}

static int mmc_send_op_cond_iter(struct mmc_drv *mmc, int use_arg)
{
	struct mmc_cmd cmd;
	int err;

	cmd.cmdidx = MMC_CMD_SEND_OP_COND;
	cmd.resp_type = MMC_RSP_R3;
	cmd.cmdarg = 0;
	if (use_arg && !mmc_host_is_spi(mmc))
		cmd.cmdarg = OCR_HCS |
			(mmc->dev->voltages &
			(mmc->dev->ocr & OCR_VOLTAGE_MASK)) |
			(mmc->dev->ocr & OCR_ACCESS_MODE);

	err = mmc_send_cmd(mmc, &cmd, NULL);
	if (err)
		return err;
	mmc->dev->ocr = cmd.response[0];
	return 0;
}

static int mmc_switch(struct mmc_drv *mmc, uint8_t set, uint8_t index, uint8_t value,
			bool send_status)
{
	absolute_time_t start;
	unsigned int status;
	struct mmc_cmd cmd;
	int timeout_ms = DEFAULT_CMD6_TIMEOUT_MS;
    absolute_time_t timeout_time = make_timeout_time_ms(timeout_ms);
	bool is_part_switch = (set == EXT_CSD_CMD_SET_NORMAL) &&
			      (index == EXT_CSD_PART_CONF);
	int ret;

    if (mmc->dev->gen_cmd6_time)
		timeout_ms = mmc->dev->gen_cmd6_time * 10;

	if (is_part_switch && mmc->dev->part_switch_time)
		timeout_ms = mmc->dev->part_switch_time * 10;

	cmd.cmdidx = MMC_CMD_SWITCH;
	cmd.resp_type = MMC_RSP_R1b;
	cmd.cmdarg = (MMC_SWITCH_MODE_WRITE_BYTE << 24) |
				 (index << 16) |
				 (value << 8);

	ret = mmc_send_cmd_retry(mmc, &cmd, NULL, 3);
	if (ret)
		return ret;
    
    start = get_absolute_time();

	/* poll dat0 for rdy/buys status */
	ret = mmc_wait_dat0(mmc, 1, timeout_ms * 1000);
	if (ret && ret != -ENOSYS)
		return ret;

	/*
	 * In cases when neiter allowed to poll by using CMD13 nor we are
	 * capable of polling by using mmc_wait_dat0, then rely on waiting the
	 * stated timeout to be sufficient.
	 */
	if (ret == -ENOSYS && !send_status) {
		sleep_ms(timeout_ms);
		return 0;
	}

	if (!send_status)
		return 0;

	/* Finally wait until the card is ready or indicates a failure
	 * to switch. It doesn't hurt to use CMD13 here even if send_status
	 * is false, because by now (after 'timeout_ms' ms) the bus should be
	 * reliable.
	 */
	do {
		ret = mmc_send_status(mmc, &status);

		if (!ret && (status & MMC_STATUS_SWITCH_ERROR)) {
			log_debug("switch failed %d/%d/0x%x !\n", set, index,
				 value);
			return -EIO;
		}
		if (!ret && (status & MMC_STATUS_RDY_FOR_DATA) &&
		    (status & MMC_STATUS_CURR_STATE) == MMC_STATE_TRANS)
			return 0;
		sleep_us(100);
	} while (0 < absolute_time_diff_us(get_absolute_time(), timeout_time));

	return -ETIMEDOUT;
}

static int mmc_complete_op_cond(struct mmc_drv *mmc)
{
    absolute_time_t start;
	struct mmc_cmd cmd;
	int timeout_ms = 1000;
    absolute_time_t timeout_time = make_timeout_time_ms(timeout_ms);
	int err;

	mmc->dev->op_cond_pending = 0;
	if (!(mmc->dev->ocr & OCR_BUSY)) {
		/* Some cards seem to need this */
		mmc_go_idle(mmc);

		start = get_absolute_time();
		while (1) {
			err = mmc_send_op_cond_iter(mmc, 1);
			if (err)
				return err;
			if (mmc->dev->ocr & OCR_BUSY)
				break;
			if (0 < absolute_time_diff_us(get_absolute_time(), timeout_time))
				return -EOPNOTSUPP;
			sleep_us(100);
		}
	}

	if (mmc_host_is_spi(mmc)) { /* read OCR for spi */
		cmd.cmdidx = MMC_CMD_SPI_READ_OCR;
		cmd.resp_type = MMC_RSP_R3;
		cmd.cmdarg = 0;

		err = mmc_send_cmd(mmc, &cmd, NULL);

		if (err)
			return err;

		mmc->dev->ocr = cmd.response[0];
	}

	mmc->dev->version = MMC_VERSION_UNKNOWN;

	mmc->dev->high_capacity = ((mmc->dev->ocr & OCR_HCS) == OCR_HCS);
	mmc->dev->rca = 1;

	return 0;
}


#if IS_ENABLED(CONFIG_MMC_IO_VOLTAGE)
static int mmc_set_signal_voltage(struct mmc_drv *mmc, uint signal_voltage)
{
	int err;

	if (mmc->dev->signal_voltage == signal_voltage)
		return 0;

	mmc->dev->signal_voltage = signal_voltage;
	err = mmc_set_ios(mmc);
	if (err)
		log_debug("unable to set voltage (err %d)\n", err);

	return err;
}

static int mmc_set_lowest_voltage(struct mmc_drv *mmc, enum bus_mode mode,
				  uint32_t allowed_mask)
{
	uint32_t card_mask = 0;

	switch (mode) {
	case MMC_HS_400_ES:
	case MMC_HS_400:
	case MMC_HS_200:
		if (mmc->dev->cardtype & (EXT_CSD_CARD_TYPE_HS200_1_8V |
		    EXT_CSD_CARD_TYPE_HS400_1_8V))
			card_mask |= MMC_SIGNAL_VOLTAGE_180;
		if (mmc->dev->cardtype & (EXT_CSD_CARD_TYPE_HS200_1_2V |
		    EXT_CSD_CARD_TYPE_HS400_1_2V))
			card_mask |= MMC_SIGNAL_VOLTAGE_120;
		break;
	case MMC_DDR_52:
		if (mmc->dev->cardtype & EXT_CSD_CARD_TYPE_DDR_1_8V)
			card_mask |= MMC_SIGNAL_VOLTAGE_330 |
				     MMC_SIGNAL_VOLTAGE_180;
		if (mmc->dev->cardtype & EXT_CSD_CARD_TYPE_DDR_1_2V)
			card_mask |= MMC_SIGNAL_VOLTAGE_120;
		break;
	default:
		card_mask |= MMC_SIGNAL_VOLTAGE_330;
		break;
	}

	while (card_mask & allowed_mask) {
		enum mmc_voltage best_match;

		best_match = 1 << (__ffs(card_mask & allowed_mask) - 1);
		if (!mmc_set_signal_voltage(mmc,  best_match))
			return 0;

		allowed_mask &= ~best_match;
	}

	return -ENOTSUP;
}

#else
static inline int mmc_set_signal_voltage(struct mmc *mmc, uint signal_voltage)
{
	return 0;
}

static inline int mmc_set_lowest_voltage(struct mmc *mmc, enum bus_mode mode,
					 uint32_t allowed_mask)
{
	return 0;
}
#endif

static int mmc_set_bus_width(struct mmc_drv *mmc, uint width)
{
	mmc->dev->bus_width = width;

	return mmc_set_ios(mmc);
}

static int mmc_set_card_speed(struct mmc_dev *mmc, enum bus_mode mode,
			      bool hsdowngrade)
{
	int err;
	int speed_bits;

	uint8_t *test_csd = calloc(MMC_MAX_BLOCK_LEN, sizeof(uint8_t));

	switch (mode) {
	case MMC_HS:
	case MMC_HS_52:
	case MMC_DDR_52:
		speed_bits = EXT_CSD_TIMING_HS;
		break;
#if IS_ENABLED(CONFIG_MMC_HS200_SUPPORT)
	case MMC_HS_200:
		speed_bits = EXT_CSD_TIMING_HS200;
		break;
#endif
#if IS_ENABLED(CONFIG_MMC_HS400_SUPPORT)
	case MMC_HS_400:
		speed_bits = EXT_CSD_TIMING_HS400;
		break;
#endif
#if IS_ENABLED(CONFIG_MMC_HS400_ES_SUPPORT)
	case MMC_HS_400_ES:
		speed_bits = EXT_CSD_TIMING_HS400;
		break;
#endif
	case MMC_LEGACY:
		speed_bits = EXT_CSD_TIMING_LEGACY;
		break;
	default:
		return -EINVAL;
	}

	err = mmc_switch(mmc, EXT_CSD_CMD_SET_NORMAL, EXT_CSD_HS_TIMING,
			   speed_bits, !hsdowngrade);
	if (err)
		return err;

#if IS_ENABLED(CONFIG_MMC_HS200_SUPPORT) || \
    IS_ENABLED(CONFIG_MMC_HS400_SUPPORT)
	/*
	 * In case the eMMC is in HS200/HS400 mode and we are downgrading
	 * to HS mode, the card clock are still running much faster than
	 * the supported HS mode clock, so we can not reliably read out
	 * Extended CSD. Reconfigure the controller to run at HS mode.
	 */
	if (hsdowngrade) {
		mmc_select_mode(mmc, MMC_HS_52);
		mmc_set_clock(mmc, mmc_mode2freq(mmc, MMC_HS_52), false);
	}
#endif

	if ((mode == MMC_HS) || (mode == MMC_HS_52)) {
		/* Now check to see that it worked */
		err = mmc_send_ext_csd(mmc, test_csd);
		if (err)
			return err;

		/* No high-speed support */
		if (!test_csd[EXT_CSD_HS_TIMING])
			return -ENOTSUP;
	}

	return 0;
}

static int mmc_get_capabilities(struct mmc_drv *mmc)
{
	uint8_t *ext_csd = mmc->dev->ext_csd;
	char cardtype;

	mmc->dev->card_caps = MMC_MODE_1BIT | MMC_CAP(MMC_LEGACY);

	if (mmc_host_is_spi(mmc))
		return 0;

	/* Only version 4 supports high-speed */
	if (mmc->dev->version < MMC_VERSION_4)
		return 0;

	if (!ext_csd) {
		log_error("No ext_csd found!\n"); /* this should never happen */
		return -ENOTSUP;
	}

	mmc->dev->card_caps |= MMC_MODE_4BIT | MMC_MODE_8BIT;

	cardtype = ext_csd[EXT_CSD_CARD_TYPE];
	mmc->dev->cardtype = cardtype;

#if IS_ENABLED(CONFIG_MMC_HS200_SUPPORT)
	if (cardtype & (EXT_CSD_CARD_TYPE_HS200_1_2V |
			EXT_CSD_CARD_TYPE_HS200_1_8V)) {
		mmc->dev->card_caps |= MMC_MODE_HS200;
	}
#endif
#if IS_ENABLED(CONFIG_MMC_HS400_SUPPORT) || \
	IS_ENABLED(CONFIG_MMC_HS400_ES_SUPPORT)
	if (cardtype & (EXT_CSD_CARD_TYPE_HS400_1_2V |
			EXT_CSD_CARD_TYPE_HS400_1_8V)) {
		mmc->dev->card_caps |= MMC_MODE_HS400;
	}
#endif
	if (cardtype & EXT_CSD_CARD_TYPE_52) {
		if (cardtype & EXT_CSD_CARD_TYPE_DDR_52)
			mmc->dev->card_caps |= MMC_MODE_DDR_52MHz;
		mmc->dev->card_caps |= MMC_MODE_HS_52MHz;
	}
	if (cardtype & EXT_CSD_CARD_TYPE_26)
		mmc->dev->card_caps |= MMC_MODE_HS;

#if IS_ENABLED(CONFIG_MMC_HS400_ES_SUPPORT)
	if (ext_csd[EXT_CSD_STROBE_SUPPORT] &&
	    (mmc->dev->card_caps & MMC_MODE_HS400)) {
		mmc->dev->card_caps |= MMC_MODE_HS400_ES;
	}
#endif

	return 0;
}


#if IS_ENABLED(MMC_WRITE)
static int sd_read_ssr(struct mmc_drv *mmc)
{
	static const unsigned int sd_au_size[] = {
		0,		SZ_16K / 512,		SZ_32K / 512,
		SZ_64K / 512,	SZ_128K / 512,		SZ_256K / 512,
		SZ_512K / 512,	SZ_1M / 512,		SZ_2M / 512,
		SZ_4M / 512,	SZ_8M / 512,		(SZ_8M + SZ_4M) / 512,
		SZ_16M / 512,	(SZ_16M + SZ_8M) / 512,	SZ_32M / 512,
		SZ_64M / 512,
	};
	int err, i;
	struct mmc_cmd cmd;
	uint32_t *ssr = calloc (16, sizeof(uint32_t));
	struct mmc_data data;
	unsigned int au, eo, et, es;

	cmd.cmdidx = MMC_CMD_APP_CMD;
	cmd.resp_type = MMC_RSP_R1;
	cmd.cmdarg = mmc->dev->rca << 16;

	err = mmc_send_cmd_quirks(mmc, &cmd, NULL, MMC_QUIRK_RETRY_APP_CMD, 4);
	if (err)
		return err;

	cmd.cmdidx = SD_CMD_APP_SD_STATUS;
	cmd.resp_type = MMC_RSP_R1;
	cmd.cmdarg = 0;

	data.dest = (char *)ssr;
	data.blocksize = 64;
	data.blocks = 1;
	data.flags = MMC_DATA_READ;

	err = mmc_send_cmd_retry(mmc, &cmd, &data, 3);
	if (err)
		return err;

	for (i = 0; i < 16; i++)
		ssr[i] = be32_to_cpu(ssr[i]);

	au = (ssr[2] >> 12) & 0xF;
	if ((au <= 9) || (mmc->dev->version == SD_VERSION_3)) {
		mmc->dev->ssr.au = sd_au_size[au];
		es = (ssr[3] >> 24) & 0xFF;
		es |= (ssr[2] & 0xFF) << 8;
		et = (ssr[3] >> 18) & 0x3F;
		if (es && et) {
			eo = (ssr[3] >> 16) & 0x3;
			mmc->dev->ssr.erase_timeout = (et * 1000) / es;
			mmc->dev->ssr.erase_offset = eo * 1000;
		}
	} else {
		log_debug("Invalid Allocation Unit Size.\n");
	}

	return 0;
}
#endif

static int sd_get_capabilities(struct mmc_drv *mmc)
{
	int err;
	struct mmc_cmd cmd;
	struct mmc_data data;
    uint32_t *scr = calloc(2, sizeof(uint32_t));
    uint32_t *switch_status = calloc(16, sizeof(uint32_t));
	int retries;
#if IS_ENABLED(CONFIG_MMC_UHS_SUPPORT)
	u32 sd3_bus_mode;
#endif

	mmc->dev->card_caps = MMC_MODE_1BIT | MMC_CAP(MMC_LEGACY);

	if (mmc_host_is_spi(mmc))
		return 0;

	/* Read the SCR to find out if this card supports higher speeds */
	cmd.cmdidx = MMC_CMD_APP_CMD;
	cmd.resp_type = MMC_RSP_R1;
	cmd.cmdarg = mmc->dev->rca << 16;

	err = mmc_send_cmd(mmc, &cmd, NULL);

	if (err)
		return err;

	cmd.cmdidx = SD_CMD_APP_SEND_SCR;
	cmd.resp_type = MMC_RSP_R1;
	cmd.cmdarg = 0;

	data.dest = (char *)scr;
	data.blocksize = 8;
	data.blocks = 1;
	data.flags = MMC_DATA_READ;

	err = mmc_send_cmd_retry(mmc, &cmd, &data, 3);

	if (err)
		return err;

	mmc->dev->scr[0] = __be32_to_cpu(scr[0]);
	mmc->dev->scr[1] = __be32_to_cpu(scr[1]);

	switch ((mmc->dev->scr[0] >> 24) & 0xf) {
	case 0:
		mmc->dev->version = SD_VERSION_1_0;
		break;
	case 1:
		mmc->dev->version = SD_VERSION_1_10;
		break;
	case 2:
		mmc->dev->version = SD_VERSION_2;
		if ((mmc->dev->scr[0] >> 15) & 0x1)
			mmc->dev->version = SD_VERSION_3;
		break;
	default:
		mmc->dev->version = SD_VERSION_1_0;
		break;
	}

	if (mmc->dev->scr[0] & SD_DATA_4BIT)
		mmc->dev->card_caps |= MMC_MODE_4BIT;

	/* Version 1.0 doesn't support switching */
	if (mmc->dev->version == SD_VERSION_1_0)
		return 0;

	retries = 4;
	while (retries--) {
		err = sd_switch(mmc, SD_SWITCH_CHECK, 0, 1,
				(uint8_t *)switch_status);

		if (err)
			return err;

		/* The high-speed function is busy.  Try again */
		if (!(__be32_to_cpu(switch_status[7]) & SD_HIGHSPEED_BUSY))
			break;
	}

	/* If high-speed isn't supported, we return */
	if (__be32_to_cpu(switch_status[3]) & SD_HIGHSPEED_SUPPORTED)
		mmc->dev->card_caps |= MMC_CAP(SD_HS);

#if IS_ENABLED(CONFIG_MMC_UHS_SUPPORT)
	/* Version before 3.0 don't support UHS modes */
	if (mmc->version < SD_VERSION_3)
		return 0;

	sd3_bus_mode = __be32_to_cpu(switch_status[3]) >> 16 & 0x1f;
	if (sd3_bus_mode & SD_MODE_UHS_SDR104)
		mmc->card_caps |= MMC_CAP(UHS_SDR104);
	if (sd3_bus_mode & SD_MODE_UHS_SDR50)
		mmc->card_caps |= MMC_CAP(UHS_SDR50);
	if (sd3_bus_mode & SD_MODE_UHS_SDR25)
		mmc->card_caps |= MMC_CAP(UHS_SDR25);
	if (sd3_bus_mode & SD_MODE_UHS_SDR12)
		mmc->card_caps |= MMC_CAP(UHS_SDR12);
	if (sd3_bus_mode & SD_MODE_UHS_DDR50)
		mmc->card_caps |= MMC_CAP(UHS_DDR50);
#endif

	return 0;
}

static int sd_select_mode_and_width(struct mmc_drv *mmc, uint32_t card_caps)
{
	int err;
	uint widths[] = {MMC_MODE_4BIT, MMC_MODE_1BIT};
	const struct mode_width_tuning *mwt;
#if IS_ENABLED(CONFIG_MMC_UHS_SUPPORT)
	bool uhs_en = (mmc->dev->ocr & OCR_S18R) ? true : false;
#else
	bool uhs_en = false;
#endif
	uint caps;

	if (mmc_host_is_spi(mmc)) {
		mmc_set_bus_width(mmc, 1);
		mmc_select_mode(mmc, MMC_LEGACY);
		mmc_set_clock(mmc, mmc->dev->tran_speed, false);
#if IS_ENABLED(CONFIG_MMC_WRITE)
		err = sd_read_ssr(mmc);
		if (err)
			log_warning("unable to read ssr\n");
#endif
		return 0;
	}

	/* Restrict card's capabilities by what the host can do */
	caps = card_caps & mmc->dev->host_caps;

	if (!uhs_en)
		caps &= ~UHS_CAPS;

	for_each_sd_mode_by_pref(caps, mwt) {
		uint *w;

		for (w = widths; w < widths + ARRAY_SIZE(widths); w++) {
			if (*w & caps & mwt->widths) {
				pr_debug("trying mode %s width %d (at %d MHz)\n",
					 mmc_mode_name(mwt->mode),
					 bus_width(*w),
					 mmc_mode2freq(mmc, mwt->mode) / 1000000);

				/* configure the bus width (card + host) */
				err = sd_select_bus_width(mmc, bus_width(*w));
				if (err)
					goto error;
				mmc_set_bus_width(mmc, bus_width(*w));

				/* configure the bus mode (card) */
				err = sd_set_card_speed(mmc, mwt->mode);
				if (err)
					goto error;

				/* configure the bus mode (host) */
				mmc_select_mode(mmc, mwt->mode);
				mmc_set_clock(mmc, mmc->dev->tran_speed,
						false);

#if IS_ENABLED(CONFIG_MMC_SUPPORTS_TUNING)
				/* execute tuning if needed */
				if (mwt->tuning && !mmc_host_is_spi(mmc->dev)) {
					err = mmc_execute_tuning(mmc,
								 mwt->tuning);
					if (err) {
						log_debug("tuning failed\n");
						goto error;
					}
				}
#endif

#if IS_ENABLED(CONFIG_MMC_WRITE)
				err = sd_read_ssr(mmc);
				if (err)
					log_warning("unable to read ssr\n");
#endif
				if (!err)
					return 0;

error:
				/* revert to a safer bus speed */
				mmc_select_mode(mmc, MMC_LEGACY);
				mmc_set_clock(mmc, mmc->dev->tran_speed,
						false);
			}
		}
	}

	log_error("unable to select a mode\n");
	return -ENOTSUP;
}

#define for_each_supported_width(caps, ddr, ecbv) \
	for (ecbv = ext_csd_bus_width;\
	    ecbv < ext_csd_bus_width + ARRAY_SIZE(ext_csd_bus_width);\
	    ecbv++) \
		if ((ddr == ecbv->is_ddr) && (caps & ecbv->cap))

static int mmc_select_mode_and_width(struct mmc_drv *mmc, uint card_caps)
{
	int err = 0;
	const struct mode_width_tuning *mwt;
	const struct ext_csd_bus_width *ecbw;

	if (mmc_host_is_spi(mmc)) {
		mmc_set_bus_width(mmc, 1);
		mmc_select_mode(mmc, MMC_LEGACY);
		mmc_set_clock(mmc, mmc->dev->tran_speed, false);
		return 0;
	}

	/* Restrict card's capabilities by what the host can do */
	card_caps &= mmc->dev->host_caps;

	/* Only version 4 of MMC supports wider bus widths */
	if (mmc->dev->version < MMC_VERSION_4)
		return 0;

	if (!mmc->dev->ext_csd) {
		log_err("No ext_csd found!\n"); /* this should enver happen */
		return -ENOTSUP;
	}

#if IS_ENABLED(CONFIG_MMC_HS200_SUPPORT) || \
    IS_ENABLED(CONFIG_MMC_HS400_SUPPORT) || \
    IS_ENABLED(CONFIG_MMC_HS400_ES_SUPPORT)
	/*
	 * In case the eMMC is in HS200/HS400 mode, downgrade to HS mode
	 * before doing anything else, since a transition from either of
	 * the HS200/HS400 mode directly to legacy mode is not supported.
	 */
	if (mmc->dev->selected_mode == MMC_HS_200 ||
	    mmc->dev->selected_mode == MMC_HS_400 ||
	    mmc->dev->selected_mode == MMC_HS_400_ES)
		mmc_set_card_speed(mmc, MMC_HS, true);
	else
#endif
		mmc_set_clock(mmc, mmc->dev->legacy_speed, false);

	for_each_mmc_mode_by_pref(card_caps, mwt) {
		for_each_supported_width(card_caps & mwt->widths,
					 mmc_is_mode_ddr(mwt->mode), ecbw) {
			enum mmc_voltage old_voltage;
			pr_debug("trying mode %s width %d (at %d MHz)\n",
				 mmc_mode_name(mwt->mode),
				 bus_width(ecbw->cap),
				 mmc_mode2freq(mmc, mwt->mode) / 1000000);
			old_voltage = mmc->dev->signal_voltage;
			err = mmc_set_lowest_voltage(mmc, mwt->mode,
						     MMC_ALL_SIGNAL_VOLTAGE);
			if (err)
				continue;

			/* configure the bus width (card + host) */
			err = mmc_switch(mmc, EXT_CSD_CMD_SET_NORMAL,
				    EXT_CSD_BUS_WIDTH,
				    ecbw->ext_csd_bits & ~EXT_CSD_DDR_FLAG, true);
			if (err)
				goto error;
			mmc_set_bus_width(mmc, bus_width(ecbw->cap));

			if (mwt->mode == MMC_HS_400) {
				err = mmc_select_hs400(mmc);
				if (err) {
					printf("Select HS400 failed %d\n", err);
					goto error;
				}
			} else if (mwt->mode == MMC_HS_400_ES) {
				err = mmc_select_hs400es(mmc);
				if (err) {
					printf("Select HS400ES failed %d\n",
					       err);
					goto error;
				}
			} else {
				/* configure the bus speed (card) */
				err = mmc_set_card_speed(mmc, mwt->mode, false);
				if (err)
					goto error;

				/*
				 * configure the bus width AND the ddr mode
				 * (card). The host side will be taken care
				 * of in the next step
				 */
				if (ecbw->ext_csd_bits & EXT_CSD_DDR_FLAG) {
					err = mmc_switch(mmc,
							 EXT_CSD_CMD_SET_NORMAL,
							 EXT_CSD_BUS_WIDTH,
							 ecbw->ext_csd_bits, true);
					if (err)
						goto error;
				}

				/* configure the bus mode (host) */
				mmc_select_mode(mmc, mwt->mode);
				mmc_set_clock(mmc, mmc->dev->tran_speed, false);
#if IS_ENABLED(CONFIG_MMC_SUPPORTS_TUNING)

				/* execute tuning if needed */
				if (mwt->tuning) {
					err = mmc_execute_tuning(mmc,
								 mwt->tuning);
					if (err) {
						log_debug("tuning failed : %d\n", err);
						goto error;
					}
				}
#endif
			}

			/* do a transfer to check the configuration */
			err = mmc_read_and_compare_ext_csd(mmc);
			if (!err)
				return 0;
error:
			mmc_set_signal_voltage(mmc, old_voltage);
			/* if an error occurred, revert to a safer bus mode */
			mmc_switch(mmc, EXT_CSD_CMD_SET_NORMAL,
				   EXT_CSD_BUS_WIDTH, EXT_CSD_BUS_WIDTH_1, true);
			mmc_select_mode(mmc, MMC_LEGACY);
			mmc_set_clock(mmc, mmc->dev->legacy_speed, false);
			mmc_set_bus_width(mmc, 1);
		}
	}

	log_error("unable to select a mode: %d\n", err);

	return -ENOTSUP;
}

int mmc_send_ext_csd(struct mmc *mmc, uint8_t *ext_csd)
{
	struct mmc_cmd cmd;
	struct mmc_data data;
	int err;

	/* Get the Card Status Register */
	cmd.cmdidx = MMC_CMD_SEND_EXT_CSD;
	cmd.resp_type = MMC_RSP_R1;
	cmd.cmdarg = 0;

	data.dest = (char *)ext_csd;
	data.blocks = 1;
	data.blocksize = MMC_MAX_BLOCK_LEN;
	data.flags = MMC_DATA_READ;

	err = mmc_send_cmd(mmc, &cmd, &data);

	return err;
}

static int mmc_startup_v4(struct mmc_drv *mmc)
{
	int err, i;
	uint64_t capacity;
	bool has_parts = false;
	bool part_completed;
	static const uint32_t mmc_versions[] = {
		MMC_VERSION_4,
		MMC_VERSION_4_1,
		MMC_VERSION_4_2,
		MMC_VERSION_4_3,
		MMC_VERSION_4_4,
		MMC_VERSION_4_41,
		MMC_VERSION_4_5,
		MMC_VERSION_5_0,
		MMC_VERSION_5_1
	};
    uint8_t *ext_csd = (uint8_t *)calloc(MMC_MAX_BLOCK_LEN, sizeof(uint8_t));

	if (IS_SD(mmc->dev) || (mmc->dev->version < MMC_VERSION_4))
		return 0;

	/* check  ext_csd version and capacity */
	err = mmc_send_ext_csd(mmc, ext_csd);
	if (err)
		goto error;

	/* store the ext csd for future reference */
	if (!mmc->dev->ext_csd)
		mmc->dev->ext_csd = calloc(MMC_MAX_BLOCK_LEN, sizeof(uint8_t));
	if (!mmc->dev->ext_csd)
		return -ENOMEM;
	memcpy(mmc->dev->ext_csd, ext_csd, MMC_MAX_BLOCK_LEN);

    if (ext_csd[EXT_CSD_REV] >= ARRAY_SIZE(mmc_versions))
		return -EINVAL;

	mmc->dev->version = mmc_versions[ext_csd[EXT_CSD_REV]];

	if (mmc->dev->version >= MMC_VERSION_4_2) {
		/*
		 * According to the JEDEC Standard, the value of
		 * ext_csd's capacity is valid if the value is more
		 * than 2GB
		 */
		capacity = ext_csd[EXT_CSD_SEC_CNT] << 0
				| ext_csd[EXT_CSD_SEC_CNT + 1] << 8
				| ext_csd[EXT_CSD_SEC_CNT + 2] << 16
				| ext_csd[EXT_CSD_SEC_CNT + 3] << 24;
		capacity *= MMC_MAX_BLOCK_LEN;
		if (mmc->dev->high_capacity)
			mmc->dev->capacity = capacity;
	}

	if (mmc->dev->version >= MMC_VERSION_4_5)
		mmc->dev->gen_cmd6_time = ext_csd[EXT_CSD_GENERIC_CMD6_TIME];

	/* The partition data may be non-zero but it is only
	 * effective if PARTITION_SETTING_COMPLETED is set in
	 * EXT_CSD, so ignore any data if this bit is not set,
	 * except for enabling the high-capacity group size
	 * definition (see below).
	 */
	part_completed = !!(ext_csd[EXT_CSD_PARTITION_SETTING] &
			    EXT_CSD_PARTITION_SETTING_COMPLETED);

	mmc->dev->part_switch_time = ext_csd[EXT_CSD_PART_SWITCH_TIME];
	/* Some eMMC set the value too low so set a minimum */
	if (mmc->dev->part_switch_time < MMC_MIN_PART_SWITCH_TIME && mmc->dev->part_switch_time)
		mmc->dev->part_switch_time = MMC_MIN_PART_SWITCH_TIME;

	/* store the partition info of emmc */
	mmc->dev->part_support = ext_csd[EXT_CSD_PARTITIONING_SUPPORT];
	if ((ext_csd[EXT_CSD_PARTITIONING_SUPPORT] & PART_SUPPORT) ||
	    ext_csd[EXT_CSD_BOOT_MULT])
		mmc->dev->part_config = ext_csd[EXT_CSD_PART_CONF];
	if (part_completed &&
	    (ext_csd[EXT_CSD_PARTITIONING_SUPPORT] & ENHNCD_SUPPORT))
		mmc->dev->part_attr = ext_csd[EXT_CSD_PARTITIONS_ATTRIBUTE];

	/*
	 * Host needs to enable ERASE_GRP_DEF bit if device is
	 * partitioned. This bit will be lost every time after a reset
	 * or power off. This will affect erase size.
	 */
	if (part_completed)
		has_parts = true;
	if ((ext_csd[EXT_CSD_PARTITIONING_SUPPORT] & PART_SUPPORT) &&
	    (ext_csd[EXT_CSD_PARTITIONS_ATTRIBUTE] & PART_ENH_ATTRIB))
		has_parts = true;
	if (has_parts) {
		err = mmc_switch(mmc, EXT_CSD_CMD_SET_NORMAL,
				 EXT_CSD_ERASE_GROUP_DEF, 1, true);

		if (err)
			goto error;

		ext_csd[EXT_CSD_ERASE_GROUP_DEF] = 1;
	}

	if (ext_csd[EXT_CSD_ERASE_GROUP_DEF] & 0x01) {
#if IS_ENABLED(CONFIG_MMC_WRITE)
		/* Read out group size from ext_csd */
		mmc->erase_grp_size =
			ext_csd[EXT_CSD_HC_ERASE_GRP_SIZE] * 1024;
#endif
		/*
		 * if high capacity and partition setting completed
		 * SEC_COUNT is valid even if it is smaller than 2 GiB
		 * JEDEC Standard JESD84-B45, 6.2.4
		 */
		if (mmc->dev->high_capacity && part_completed) {
			capacity = (ext_csd[EXT_CSD_SEC_CNT]) |
				(ext_csd[EXT_CSD_SEC_CNT + 1] << 8) |
				(ext_csd[EXT_CSD_SEC_CNT + 2] << 16) |
				(ext_csd[EXT_CSD_SEC_CNT + 3] << 24);
			capacity *= MMC_MAX_BLOCK_LEN;
			mmc->dev->capacity = capacity;
		}
	}
#if IS_ENABLED(CONFIG_MMC_WRITE)
	else {
		/* Calculate the group size from the csd value. */
		int erase_gsz, erase_gmul;

		erase_gsz = (mmc->csd[2] & 0x00007c00) >> 10;
		erase_gmul = (mmc->csd[2] & 0x000003e0) >> 5;
		mmc->dev->erase_grp_size = (erase_gsz + 1)
			* (erase_gmul + 1);
	}
#endif
#if IS_ENABLED(CONFIG:MMC_HW_PARTITIONING)
	mmc->dev->hc_wp_grp_size = 1024
		* ext_csd[EXT_CSD_HC_ERASE_GRP_SIZE]
		* ext_csd[EXT_CSD_HC_WP_GRP_SIZE];
#endif

	mmc->dev->wr_rel_set = ext_csd[EXT_CSD_WR_REL_SET];

	mmc->dev->can_trim =
		!!(ext_csd[EXT_CSD_SEC_FEATURE] & EXT_CSD_SEC_FEATURE_TRIM_EN);

	return 0;

error:
	if (mmc->dev->ext_csd)
		free(mmc->dev->ext_csd);

	return err;
}

static int mmc_startup(struct mmc_drv *mmc) {
    int err, i;
	uint mult, freq;
	uint64_t cmult, csize;
	struct mmc_cmd cmd;
	struct blk_desc *bdesc;

#ifdef CONFIG_MMC_SPI_CRC_ON
	if (mmc_host_is_spi(mmc->dev)) { /* enable CRC check for spi */
		cmd.cmdidx = MMC_CMD_SPI_CRC_ON_OFF;
		cmd.resp_type = MMC_RSP_R1;
		cmd.cmdarg = 1;
		err = mmc_send_cmd(mmc, &cmd, NULL);
		if (err)
			return err;
	}
#endif

	/* Put the Card in Identify Mode */
	cmd.cmdidx = mmc_host_is_spi(mmc) ? MMC_CMD_SEND_CID :
		MMC_CMD_ALL_SEND_CID; /* cmd not supported in spi */
	cmd.resp_type = MMC_RSP_R2;
	cmd.cmdarg = 0;

	err = mmc_send_cmd_quirks(mmc, &cmd, NULL, MMC_QUIRK_RETRY_SEND_CID, 4);
	if (err)
		return err;

	memcpy(mmc->dev->cid, cmd.response, 16);

	/*
	 * For MMC cards, set the Relative Address.
	 * For SD cards, get the Relative Address.
	 * This also puts the cards into Standby State
	 */
	if (!mmc_host_is_spi(mmc)) { /* cmd not supported in spi */
		cmd.cmdidx = SD_CMD_SEND_RELATIVE_ADDR;
		cmd.cmdarg = mmc->dev->rca << 16;
		cmd.resp_type = MMC_RSP_R6;

		err = mmc_send_cmd(mmc, &cmd, NULL);

		if (err)
			return err;

		if (IS_SD(mmc->dev))
			mmc->dev->rca = (cmd.response[0] >> 16) & 0xffff;
	}

	/* Get the Card-Specific Data */
	cmd.cmdidx = MMC_CMD_SEND_CSD;
	cmd.resp_type = MMC_RSP_R2;
	cmd.cmdarg = mmc->dev->rca << 16;

	err = mmc_send_cmd(mmc, &cmd, NULL);

	if (err)
		return err;

	mmc->dev->csd[0] = cmd.response[0];
	mmc->dev->csd[1] = cmd.response[1];
	mmc->dev->csd[2] = cmd.response[2];
	mmc->dev->csd[3] = cmd.response[3];

	if (mmc->dev->version == MMC_VERSION_UNKNOWN) {
		int version = (cmd.response[0] >> 26) & 0xf;

		switch (version) {
		case 0:
			mmc->dev->version = MMC_VERSION_1_2;
			break;
		case 1:
			mmc->dev->version = MMC_VERSION_1_4;
			break;
		case 2:
			mmc->dev->version = MMC_VERSION_2_2;
			break;
		case 3:
			mmc->dev->version = MMC_VERSION_3;
			break;
		case 4:
			mmc->dev->version = MMC_VERSION_4;
			break;
		default:
			mmc->dev->version = MMC_VERSION_1_2;
			break;
		}
	}

	/* divide frequency by 10, since the mults are 10x bigger */
	freq = fbase[(cmd.response[0] & 0x7)];
	mult = multipliers[((cmd.response[0] >> 3) & 0xf)];

	mmc->dev->legacy_speed = freq * mult;
	if (!mmc->dev->legacy_speed)
		log_debug("TRAN_SPEED: reserved value");
	mmc_select_mode(mmc, MMC_LEGACY);

	mmc->dev->dsr_imp = ((cmd.response[1] >> 12) & 0x1);
	mmc->dev->read_bl_len = 1 << ((cmd.response[1] >> 16) & 0xf);
#if IS_ENABLED(CONFIG_MMC_WRITE)

	if (IS_SD(mmc))
		mmc->dev->write_bl_len = mmc->dev->read_bl_len;
	else
		mmc->dev->write_bl_len = 1 << ((cmd.response[3] >> 22) & 0xf);
#endif

	if (mmc->dev->high_capacity) {
		csize = (mmc->dev->csd[1] & 0x3f) << 16
			| (mmc->dev->csd[2] & 0xffff0000) >> 16;
		cmult = 8;
	} else {
		csize = (mmc->dev->csd[1] & 0x3ff) << 2
			| (mmc->dev->csd[2] & 0xc0000000) >> 30;
		cmult = (mmc->dev->csd[2] & 0x00038000) >> 15;
	}

	mmc->dev->capacity = (csize + 1) << (cmult + 2);
	mmc->dev->capacity *= mmc->dev->read_bl_len;

	if (mmc->dev->read_bl_len > MMC_MAX_BLOCK_LEN)
		mmc->dev->read_bl_len = MMC_MAX_BLOCK_LEN;

#if IS_ENABLED(CONFIG_MMC_WRITE)
	if (mmc->write_bl_len > MMC_MAX_BLOCK_LEN)
		mmc->write_bl_len = MMC_MAX_BLOCK_LEN;
#endif

	if ((mmc->dev->dsr_imp) && (0xffffffff != mmc->dev->dsr)) {
		cmd.cmdidx = MMC_CMD_SET_DSR;
		cmd.cmdarg = (mmc->dev->dsr & 0xffff) << 16;
		cmd.resp_type = MMC_RSP_NONE;
		if (mmc_send_cmd(mmc, &cmd, NULL))
			log_warning("MMC: SET_DSR failed\n");
	}

	/* Select the card, and put it into Transfer Mode */
	if (!mmc_host_is_spi(mmc)) { /* cmd not supported in spi */
		cmd.cmdidx = MMC_CMD_SELECT_CARD;
		cmd.resp_type = MMC_RSP_R1;
		cmd.cmdarg = mmc->dev->rca << 16;
		err = mmc_send_cmd(mmc, &cmd, NULL);

		if (err)
			return err;
	}

	/*
	 * For SD, its erase group is always one sector
	 */
#if IS_ENABLED(CONFIG_MMC_WRITE)
	mmc->dev->erase_grp_size = 1;
#endif
	mmc->dev->part_config = MMCPART_NOAVAILABLE;

	err = mmc_startup_v4(mmc);
	if (err)
		return err;

	if (IS_SD(mmc->dev)) {
		err = sd_get_capabilities(mmc);
		if (err)
			return err;
		err = sd_select_mode_and_width(mmc, mmc->dev->card_caps);
	} else {
		err = mmc_get_capabilities(mmc);
		if (err)
			return err;
		err = mmc_select_mode_and_width(mmc, mmc->dev->card_caps);
	}

    if (err)
		return err;

	mmc->dev->best_mode = mmc->dev->selected_mode;

	/* Fix the block length for DDR mode */
	if (mmc->dev->ddr_mode) {
		mmc->dev->read_bl_len = MMC_MAX_BLOCK_LEN;
#if IS_ENABLED(CONFIG_MMC_WRITE)
		mmc->dev->write_bl_len = MMC_MAX_BLOCK_LEN;
#endif
	}

	return 0;
}

int mmc_getcd(struct mmc_drv *mmc)
{
    struct mmc_ops *ops = mmc->ops;

	if (ops->get_cd)
	    return ops->get_cd(mmc);

	return -ENOSYS;
}

static int mmc_deferred_probe(struct mmc_drv *mmc)
{
	struct mmc_ops *ops = mmc->ops;

	if (ops->deferred_probe)
		return ops->deferred_probe(mmc);

	return 0;
}

int mmc_start_init(struct mmc_drv *mmc)
{
	bool no_card;
	int err = 0;

	/*
	 * all hosts are capable of 1 bit bus-width and able to use the legacy
	 * timings.
	 */
	mmc->dev->host_caps = mmc->dev->host_caps | MMC_CAP(MMC_LEGACY) |
			 MMC_MODE_1BIT;

	if (IS_ENABLED(CONFIG_MMC_SPEED_MODE_SET)) {
		if (mmc->dev->user_speed_mode != MMC_MODES_END) {
			int i;
			/* set host caps */
			if (mmc->dev->host_caps & MMC_CAP(mmc->dev->user_speed_mode)) {
				/* Remove all existing speed capabilities */
				for (i = MMC_LEGACY; i < MMC_MODES_END; i++)
					mmc->dev->host_caps &= ~MMC_CAP(i);
				mmc->dev->host_caps |= (MMC_CAP(mmc->dev->user_speed_mode)
						   | MMC_CAP(MMC_LEGACY) |
						   MMC_MODE_1BIT);
			} else {
				log_error("bus_mode requested is not supported\n");
				return -EINVAL;
			}
		}
	}
	mmc_deferred_probe(mmc);
#if !defined(CONFIG_MMC_BROKEN_CD)
	no_card = mmc_getcd(mmc) == 0;
#else
	no_card = 0;
#endif
	if (no_card) {
		mmc->dev->initialized = false;
		log_error("MMC: no card present\n");
		return -ENOMEDIUM;
	}

	err = mmc_get_op_cond(mmc, false);

	if (!err)
		mmc->dev->init_in_progress = true;

	return err;
}

static int mmc_complete_init(struct mmc_drv *mmc)
{
	int err = 0;

	mmc->dev->init_in_progress = false;
	if (mmc->dev->op_cond_pending)
		err = mmc_complete_op_cond(mmc);

	if (!err)
		err = mmc_startup(mmc);
	if (err)
		mmc->dev->initialized = false;
	else
		mmc->dev->initialized = true;
	return err;
}

int mmc_init(struct mmc_drv *mmc)
{
    int err = 0;
	unsigned long start;

	if (mmc->dev->initialized)
		return 0;

	if (!mmc->dev->init_in_progress)
		err = mmc_start_init(mmc);

	if (!err)
		err = mmc_complete_init(mmc);
	if (err) {
		log_info("%s: %d, time %llu\n", __func__, err, get_absolute_time());
		return err;
	}

	return err;
}

int mmc_deinit(struct mmc_drv *mmc)
{
	uint32_t caps_filtered;

	if (!IS_ENABLED(CONFIG_MMC_UHS_SUPPORT) &&
	    !IS_ENABLED(CONFIG_MMC_HS200_SUPPORT) &&
	    !IS_ENABLED(CONFIG_MMC_HS400_SUPPORT))
		return 0;

	if (!mmc->dev->initialized)
		return 0;

	if (IS_SD(mmc->dev)) {
		caps_filtered = mmc->dev->card_caps &
			~(MMC_CAP(UHS_SDR12) | MMC_CAP(UHS_SDR25) |
			  MMC_CAP(UHS_SDR50) | MMC_CAP(UHS_DDR50) |
			  MMC_CAP(UHS_SDR104));

		return sd_select_mode_and_width(mmc, caps_filtered);
	} else {
		caps_filtered = mmc->dev->card_caps &
			~(MMC_CAP(MMC_HS_200) | MMC_CAP(MMC_HS_400) | MMC_CAP(MMC_HS_400_ES));

		return mmc_select_mode_and_width(mmc, caps_filtered);
	}
}

/* [] END OF FILE */