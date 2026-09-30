/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Analog Devices ADXL375 +-200 g accelerometer.
 *
 * The ADXL375 shares the ADXL345 register map (and its DEVID of 0xE5), but
 * has a fixed +-200 g range at 49 mg/LSB, so neither the upstream adxl345
 * nor adxl372 driver can be used for it.
 */

#define DT_DRV_COMPAT adi_adxl375

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(ADXL375, CONFIG_SENSOR_LOG_LEVEL);

#define ADXL375_REG_DEVID       0x00
#define ADXL375_REG_OFSX        0x1E
#define ADXL375_REG_BW_RATE     0x2C
#define ADXL375_REG_POWER_CTL   0x2D
#define ADXL375_REG_DATA_FORMAT 0x31
#define ADXL375_REG_DATAX0      0x32
#define ADXL375_REG_FIFO_CTL    0x38

#define ADXL375_DEVID 0xE5

#define ADXL375_POWER_CTL_MEASURE BIT(3)

/* Bits D3, D1 and D0 must be written as 1; right justified, 4-wire SPI. */
#define ADXL375_DATA_FORMAT 0x0B

#define ADXL375_FIFO_CTL_BYPASS 0x00

/* Sensitivity: 49 mg/LSB */
#define ADXL375_UG_PER_LSB 49000

/* BW_RATE rate code of the lowest entry; each following entry doubles */
#define ADXL375_RATE_CODE_25HZ 0x08

static const uint16_t adxl375_odr_hz[] = {25, 50, 100, 200, 400, 800, 1600, 3200};

struct adxl375_config {
	struct i2c_dt_spec i2c;
	uint16_t odr;
};

struct adxl375_data {
	int16_t x;
	int16_t y;
	int16_t z;
};

static int adxl375_set_odr(const struct device *dev, uint16_t hz)
{
	const struct adxl375_config *cfg = dev->config;

	/* Round up to the next supported rate */
	for (size_t i = 0; i < ARRAY_SIZE(adxl375_odr_hz); i++) {
		if (hz <= adxl375_odr_hz[i]) {
			return i2c_reg_write_byte_dt(&cfg->i2c, ADXL375_REG_BW_RATE,
						     ADXL375_RATE_CODE_25HZ + i);
		}
	}

	LOG_ERR("unsupported ODR %u Hz", hz);
	return -EINVAL;
}

static int adxl375_attr_set(const struct device *dev, enum sensor_channel chan,
			    enum sensor_attribute attr, const struct sensor_value *val)
{
	if (chan != SENSOR_CHAN_ACCEL_XYZ && chan != SENSOR_CHAN_ALL) {
		return -ENOTSUP;
	}

	switch (attr) {
	case SENSOR_ATTR_SAMPLING_FREQUENCY:
		if (val->val1 < 0 || val->val1 > UINT16_MAX) {
			return -EINVAL;
		}
		return adxl375_set_odr(dev, val->val1);
	default:
		return -ENOTSUP;
	}
}

static int adxl375_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	const struct adxl375_config *cfg = dev->config;
	struct adxl375_data *data = dev->data;
	uint8_t buf[6];
	int ret;

	switch (chan) {
	case SENSOR_CHAN_ALL:
	case SENSOR_CHAN_ACCEL_X:
	case SENSOR_CHAN_ACCEL_Y:
	case SENSOR_CHAN_ACCEL_Z:
	case SENSOR_CHAN_ACCEL_XYZ:
		break;
	default:
		return -ENOTSUP;
	}

	/* One burst read so all three axes come from the same sample */
	ret = i2c_burst_read_dt(&cfg->i2c, ADXL375_REG_DATAX0, buf, sizeof(buf));
	if (ret < 0) {
		LOG_ERR("failed to read sample (%d)", ret);
		return ret;
	}

	data->x = (int16_t)sys_get_le16(&buf[0]);
	data->y = (int16_t)sys_get_le16(&buf[2]);
	data->z = (int16_t)sys_get_le16(&buf[4]);

	return 0;
}

static void adxl375_convert(struct sensor_value *val, int16_t raw)
{
	sensor_ug_to_ms2((int32_t)raw * ADXL375_UG_PER_LSB, val);
}

static int adxl375_channel_get(const struct device *dev, enum sensor_channel chan,
			       struct sensor_value *val)
{
	struct adxl375_data *data = dev->data;

	switch (chan) {
	case SENSOR_CHAN_ACCEL_X:
		adxl375_convert(val, data->x);
		break;
	case SENSOR_CHAN_ACCEL_Y:
		adxl375_convert(val, data->y);
		break;
	case SENSOR_CHAN_ACCEL_Z:
		adxl375_convert(val, data->z);
		break;
	case SENSOR_CHAN_ACCEL_XYZ:
		adxl375_convert(&val[0], data->x);
		adxl375_convert(&val[1], data->y);
		adxl375_convert(&val[2], data->z);
		break;
	default:
		return -ENOTSUP;
	}

	return 0;
}

static DEVICE_API(sensor, adxl375_api) = {
	.attr_set = adxl375_attr_set,
	.sample_fetch = adxl375_sample_fetch,
	.channel_get = adxl375_channel_get,
};

static int adxl375_init(const struct device *dev)
{
	const struct adxl375_config *cfg = dev->config;
	const uint8_t no_offset[3] = {0};
	uint8_t id;
	int ret;

	if (!i2c_is_ready_dt(&cfg->i2c)) {
		LOG_ERR("bus %s not ready", cfg->i2c.bus->name);
		return -ENODEV;
	}

	ret = i2c_reg_read_byte_dt(&cfg->i2c, ADXL375_REG_DEVID, &id);
	if (ret < 0) {
		LOG_ERR("failed to read id (%d)", ret);
		return ret;
	}
	if (id != ADXL375_DEVID) {
		LOG_ERR("unexpected id 0x%02x (expected 0x%02x)", id, ADXL375_DEVID);
		return -ENODEV;
	}

	/* Configure in standby, then start measuring */
	ret = i2c_reg_write_byte_dt(&cfg->i2c, ADXL375_REG_POWER_CTL, 0);
	if (ret < 0) {
		return ret;
	}

	/*
	 * There is no soft reset and the offset registers only clear on power
	 * loss, so a stale offset would survive an MCU reset.
	 */
	ret = i2c_burst_write_dt(&cfg->i2c, ADXL375_REG_OFSX, no_offset, sizeof(no_offset));
	if (ret < 0) {
		return ret;
	}

	ret = i2c_reg_write_byte_dt(&cfg->i2c, ADXL375_REG_DATA_FORMAT, ADXL375_DATA_FORMAT);
	if (ret < 0) {
		return ret;
	}

	ret = i2c_reg_write_byte_dt(&cfg->i2c, ADXL375_REG_FIFO_CTL, ADXL375_FIFO_CTL_BYPASS);
	if (ret < 0) {
		return ret;
	}

	ret = adxl375_set_odr(dev, cfg->odr);
	if (ret < 0) {
		return ret;
	}

	return i2c_reg_write_byte_dt(&cfg->i2c, ADXL375_REG_POWER_CTL,
				     ADXL375_POWER_CTL_MEASURE);
}

#define ADXL375_DEFINE(inst)                                                                       \
	static struct adxl375_data adxl375_data_##inst;                                            \
                                                                                                   \
	static const struct adxl375_config adxl375_config_##inst = {                               \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                                 \
		.odr = DT_INST_PROP(inst, odr),                                                    \
	};                                                                                         \
                                                                                                   \
	SENSOR_DEVICE_DT_INST_DEFINE(inst, adxl375_init, NULL, &adxl375_data_##inst,               \
				     &adxl375_config_##inst, POST_KERNEL,                          \
				     CONFIG_SENSOR_INIT_PRIORITY, &adxl375_api);

DT_INST_FOREACH_STATUS_OKAY(ADXL375_DEFINE)
