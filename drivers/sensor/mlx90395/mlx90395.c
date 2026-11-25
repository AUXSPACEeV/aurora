/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Melexis MLX90395 3-axis magnetometer.
 *
 * Unlike the register based MLX90394, the MLX90395 is driven by commands
 * written to address 0x80, each answered with a status byte, and it has no
 * identification register. The upstream mlx90394 driver therefore cannot be
 * used for it. Only I2C and single measurement mode are implemented.
 */

#define DT_DRV_COMPAT melexis_mlx90395

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(MLX90395, CONFIG_SENSOR_LOG_LEVEL);

/* Commands are written here; reading from here returns status and data */
#define MLX90395_REG_CMD 0x80

#define MLX90395_CMD_SM 0x30
#define MLX90395_CMD_EX 0x80
#define MLX90395_CMD_RT 0xF0

/* zyxt argument of SM: measure Z, Y, X and T */
#define MLX90395_ZYXT_ALL 0x0F

#define MLX90395_STATUS_DRDY      BIT(0)
#define MLX90395_STATUS_RST       BIT(1)
#define MLX90395_STATUS_OVF       BIT(2)
#define MLX90395_STATUS_CE        BIT(3)
#define MLX90395_STATUS_MODE      GENMASK(7, 4)
#define MLX90395_STATUS_MODE_IDLE 0x00
#define MLX90395_STATUS_MODE_SM   0x20

/* Configuration word addresses; I2C accesses them at twice this address */
#define MLX90395_REG_CONF0 0x00
#define MLX90395_REG_CONF2 0x02

#define MLX90395_CONF0_GAIN_SEL GENMASK(7, 4)

#define MLX90395_CONF2_OSR      GENMASK(1, 0)
#define MLX90395_CONF2_DIG_FILT GENMASK(4, 2)
#define MLX90395_CONF2_RES_X    GENMASK(6, 5)
#define MLX90395_CONF2_RES_Y    GENMASK(8, 7)
#define MLX90395_CONF2_RES_Z    GENMASK(10, 9)
#define MLX90395_CONF2_OSR2     GENMASK(12, 11)

/* Factory GainSel of each variant, and its sensitivity at resolution 1 */
#define MLX90395_GAIN_SEL_50MT     9
#define MLX90395_GAIN_SEL_120MT    8
#define MLX90395_LSB_PER_MT_50MT   400
#define MLX90395_LSB_PER_MT_120MT  140

#define MLX90395_UGAUSS_PER_MT 10000000LL

/* Temperature: 50 LSB/degC with 0 LSB at 0 degC */
#define MLX90395_UDEGC_PER_LSB 20000

/* Reset time, and the longest possible conversion (all filters at maximum) */
#define MLX90395_T_POR_US      2400
#define MLX90395_T_EXIT_MAX_MS 110

/* Poll interval and count once the computed conversion time has elapsed */
#define MLX90395_POLL_US      100
#define MLX90395_POLL_RETRIES 20

struct mlx90395_config {
	struct i2c_dt_spec i2c;
	uint8_t resolution;
	uint8_t osr;
	uint8_t dig_filt;
};

struct mlx90395_data {
	int16_t x;
	int16_t y;
	int16_t z;
	int16_t t;
	uint16_t lsb_per_mt;
	uint32_t conv_time_us;
};

static int mlx90395_command(const struct device *dev, uint8_t cmd, uint8_t *status)
{
	const struct mlx90395_config *cfg = dev->config;
	const uint8_t tx[2] = {MLX90395_REG_CMD, cmd};

	return i2c_write_read_dt(&cfg->i2c, tx, sizeof(tx), status, 1);
}

static int mlx90395_read_status(const struct device *dev, uint8_t *status)
{
	const struct mlx90395_config *cfg = dev->config;

	return i2c_reg_read_byte_dt(&cfg->i2c, MLX90395_REG_CMD, status);
}

static int mlx90395_reg_read(const struct device *dev, uint8_t reg, uint16_t *val)
{
	const struct mlx90395_config *cfg = dev->config;
	uint8_t buf[2];
	int ret;

	ret = i2c_burst_read_dt(&cfg->i2c, reg << 1, buf, sizeof(buf));
	if (ret < 0) {
		return ret;
	}

	*val = sys_get_be16(buf);

	return 0;
}

static int mlx90395_reg_write(const struct device *dev, uint8_t reg, uint16_t val)
{
	const struct mlx90395_config *cfg = dev->config;
	uint8_t tx[3] = {reg << 1};
	uint8_t status;
	int ret;

	sys_put_be16(val, &tx[1]);

	ret = i2c_write_read_dt(&cfg->i2c, tx, sizeof(tx), &status, 1);
	if (ret < 0) {
		return ret;
	}

	/* In reply to memory commands, bit 3 flags an error (CE or DED) */
	if (status & MLX90395_STATUS_CE) {
		LOG_ERR("write of register 0x%02x rejected, status 0x%02x", reg, status);
		return -EIO;
	}

	return 0;
}

/* Typical time for one X, Y, Z and T measurement, plus oscillator tolerance */
static uint32_t mlx90395_conv_time_us(uint16_t conf2)
{
	uint32_t osr = FIELD_GET(MLX90395_CONF2_OSR, conf2);
	uint32_t dig_filt = FIELD_GET(MLX90395_CONF2_DIG_FILT, conf2);
	uint32_t osr2 = FIELD_GET(MLX90395_CONF2_OSR2, conf2);
	uint32_t t_mag = 67 + 32 * BIT(osr) * (2 + BIT(dig_filt));
	uint32_t t_temp = 67 + 96 * BIT(osr2);
	uint32_t t = 165 + t_temp + 3 * t_mag + 11;

	/* The main oscillator is specified to +-5 % */
	return t + t / 20;
}

static int mlx90395_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	const struct mlx90395_config *cfg = dev->config;
	struct mlx90395_data *data = dev->data;
	/* Status, CRC, then X, Y, Z, T and V, each big endian */
	uint8_t buf[12];
	uint8_t status;
	int ret;

	switch (chan) {
	case SENSOR_CHAN_ALL:
	case SENSOR_CHAN_MAGN_X:
	case SENSOR_CHAN_MAGN_Y:
	case SENSOR_CHAN_MAGN_Z:
	case SENSOR_CHAN_MAGN_XYZ:
	case SENSOR_CHAN_DIE_TEMP:
		break;
	default:
		return -ENOTSUP;
	}

	ret = mlx90395_command(dev, MLX90395_CMD_SM | MLX90395_ZYXT_ALL, &status);
	if (ret < 0) {
		LOG_ERR("failed to start measurement (%d)", ret);
		return ret;
	}
	if ((status & (MLX90395_STATUS_MODE | MLX90395_STATUS_CE)) != MLX90395_STATUS_MODE_SM) {
		LOG_ERR("measurement rejected, status 0x%02x", status);
		return -EIO;
	}

	k_usleep(data->conv_time_us);

	/* The IC returns to idle with DRDY set once the measurement is done */
	for (int i = 0;; i++) {
		ret = i2c_burst_read_dt(&cfg->i2c, MLX90395_REG_CMD, buf, sizeof(buf));
		if (ret < 0) {
			LOG_ERR("failed to read measurement (%d)", ret);
			return ret;
		}

		status = buf[0];
		if (FIELD_GET(MLX90395_STATUS_MODE, status) == MLX90395_STATUS_MODE_IDLE &&
		    (status & MLX90395_STATUS_DRDY)) {
			break;
		}

		if (i == MLX90395_POLL_RETRIES) {
			LOG_ERR("measurement timed out, status 0x%02x", status);
			return -ETIMEDOUT;
		}

		k_usleep(MLX90395_POLL_US);
	}

	if (status & MLX90395_STATUS_OVF) {
		LOG_ERR("ADC overflow, status 0x%02x", status);
		return -EIO;
	}

	if (crc8_ccitt(0x00, &buf[2], 10) != buf[1]) {
		LOG_ERR("CRC mismatch");
		return -EIO;
	}

	data->x = (int16_t)sys_get_be16(&buf[2]);
	data->y = (int16_t)sys_get_be16(&buf[4]);
	data->z = (int16_t)sys_get_be16(&buf[6]);
	data->t = (int16_t)sys_get_be16(&buf[8]);

	return 0;
}

static void mlx90395_convert_magn(const struct device *dev, struct sensor_value *val,
				  int16_t raw)
{
	const struct mlx90395_config *cfg = dev->config;
	struct mlx90395_data *data = dev->data;

	/* Sensitivity halves with each resolution step above 1 */
	(void)sensor_value_from_micro(val, raw * MLX90395_UGAUSS_PER_MT * BIT(cfg->resolution) /
					      (2 * data->lsb_per_mt));
}

static int mlx90395_channel_get(const struct device *dev, enum sensor_channel chan,
				struct sensor_value *val)
{
	struct mlx90395_data *data = dev->data;

	switch (chan) {
	case SENSOR_CHAN_MAGN_X:
		mlx90395_convert_magn(dev, val, data->x);
		break;
	case SENSOR_CHAN_MAGN_Y:
		mlx90395_convert_magn(dev, val, data->y);
		break;
	case SENSOR_CHAN_MAGN_Z:
		mlx90395_convert_magn(dev, val, data->z);
		break;
	case SENSOR_CHAN_MAGN_XYZ:
		mlx90395_convert_magn(dev, &val[0], data->x);
		mlx90395_convert_magn(dev, &val[1], data->y);
		mlx90395_convert_magn(dev, &val[2], data->z);
		break;
	case SENSOR_CHAN_DIE_TEMP:
		(void)sensor_value_from_micro(val, (int64_t)data->t * MLX90395_UDEGC_PER_LSB);
		break;
	default:
		return -ENOTSUP;
	}

	return 0;
}

static DEVICE_API(sensor, mlx90395_api) = {
	.sample_fetch = mlx90395_sample_fetch,
	.channel_get = mlx90395_channel_get,
};

/*
 * Leave any mode a previous boot may have left running, then reset. The IC
 * finishes an ongoing measurement after EX, and RT is only allowed in idle.
 */
static int mlx90395_reset(const struct device *dev)
{
	const struct mlx90395_config *cfg = dev->config;
	const uint8_t rt[2] = {MLX90395_REG_CMD, MLX90395_CMD_RT};
	uint8_t status;
	int64_t deadline;
	int ret;

	ret = mlx90395_command(dev, MLX90395_CMD_EX, &status);
	if (ret < 0) {
		LOG_ERR("no response (%d)", ret);
		return ret;
	}

	deadline = k_uptime_get() + MLX90395_T_EXIT_MAX_MS;
	do {
		k_msleep(1);

		ret = mlx90395_read_status(dev, &status);
		if (ret < 0) {
			return ret;
		}
	} while (FIELD_GET(MLX90395_STATUS_MODE, status) != MLX90395_STATUS_MODE_IDLE &&
		 k_uptime_get() < deadline);

	if (FIELD_GET(MLX90395_STATUS_MODE, status) != MLX90395_STATUS_MODE_IDLE) {
		LOG_ERR("did not return to idle, status 0x%02x", status);
		return -EIO;
	}

	/* RT does not reply with a status byte */
	ret = i2c_write_dt(&cfg->i2c, rt, sizeof(rt));
	if (ret < 0) {
		LOG_ERR("reset failed (%d)", ret);
		return ret;
	}

	k_usleep(MLX90395_T_POR_US);

	/*
	 * Without an ID register, the status after reset is the best presence
	 * check: idle, reset flag set, and the command accepted.
	 */
	ret = mlx90395_command(dev, MLX90395_CMD_EX, &status);
	if (ret < 0) {
		return ret;
	}
	if ((status & (MLX90395_STATUS_MODE | MLX90395_STATUS_CE | MLX90395_STATUS_RST)) !=
	    MLX90395_STATUS_RST) {
		LOG_ERR("unexpected status 0x%02x after reset", status);
		return -ENODEV;
	}

	return 0;
}

static int mlx90395_init(const struct device *dev)
{
	const struct mlx90395_config *cfg = dev->config;
	struct mlx90395_data *data = dev->data;
	uint16_t conf0;
	uint16_t conf2;
	uint8_t gain_sel;
	int ret;

	if (!i2c_is_ready_dt(&cfg->i2c)) {
		LOG_ERR("bus %s not ready", cfg->i2c.bus->name);
		return -ENODEV;
	}

	ret = mlx90395_reset(dev);
	if (ret < 0) {
		return ret;
	}

	/* The reset recalled the factory settings, which identify the variant */
	ret = mlx90395_reg_read(dev, MLX90395_REG_CONF0, &conf0);
	if (ret < 0) {
		return ret;
	}

	gain_sel = FIELD_GET(MLX90395_CONF0_GAIN_SEL, conf0);
	switch (gain_sel) {
	case MLX90395_GAIN_SEL_50MT:
		data->lsb_per_mt = MLX90395_LSB_PER_MT_50MT;
		break;
	case MLX90395_GAIN_SEL_120MT:
		data->lsb_per_mt = MLX90395_LSB_PER_MT_120MT;
		break;
	default:
		LOG_ERR("unknown variant, GainSel %u", gain_sel);
		return -ENODEV;
	}

	ret = mlx90395_reg_read(dev, MLX90395_REG_CONF2, &conf2);
	if (ret < 0) {
		return ret;
	}

	conf2 &= ~(MLX90395_CONF2_OSR | MLX90395_CONF2_DIG_FILT | MLX90395_CONF2_RES_X |
		   MLX90395_CONF2_RES_Y | MLX90395_CONF2_RES_Z);
	conf2 |= FIELD_PREP(MLX90395_CONF2_OSR, cfg->osr) |
		 FIELD_PREP(MLX90395_CONF2_DIG_FILT, cfg->dig_filt) |
		 FIELD_PREP(MLX90395_CONF2_RES_X, cfg->resolution) |
		 FIELD_PREP(MLX90395_CONF2_RES_Y, cfg->resolution) |
		 FIELD_PREP(MLX90395_CONF2_RES_Z, cfg->resolution);

	/* Volatile only; the NVRAM is never written */
	ret = mlx90395_reg_write(dev, MLX90395_REG_CONF2, conf2);
	if (ret < 0) {
		return ret;
	}

	data->conv_time_us = mlx90395_conv_time_us(conf2);

	LOG_DBG("%u mT variant, conversion time %u us",
		data->lsb_per_mt == MLX90395_LSB_PER_MT_50MT ? 50 : 120, data->conv_time_us);

	return 0;
}

#define MLX90395_DEFINE(inst)                                                                      \
	static struct mlx90395_data mlx90395_data_##inst;                                          \
                                                                                                   \
	static const struct mlx90395_config mlx90395_config_##inst = {                             \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                                 \
		.resolution = DT_INST_PROP(inst, resolution),                                      \
		.osr = DT_INST_PROP(inst, oversampling),                                           \
		.dig_filt = DT_INST_PROP(inst, digital_filter),                                    \
	};                                                                                         \
                                                                                                   \
	SENSOR_DEVICE_DT_INST_DEFINE(inst, mlx90395_init, NULL, &mlx90395_data_##inst,             \
				     &mlx90395_config_##inst, POST_KERNEL,                         \
				     CONFIG_SENSOR_INIT_PRIORITY, &mlx90395_api);

DT_INST_FOREACH_STATUS_OKAY(MLX90395_DEFINE)
