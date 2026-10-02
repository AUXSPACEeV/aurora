/**
 * @file mag.c
 * @brief Magnetometer library implementation.
 *
 * Polls the hardware driver and publishes struct mag_data on mag_data_chan.
 * There is no simulated source; see MAG in Kconfig.
 *
 * Copyright (c) 2026, Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/zbus/zbus.h>

#include <aurora/lib/data_logger.h>
#include <aurora/lib/mag.h>

LOG_MODULE_REGISTER(mag, CONFIG_AURORA_SENSORS_LOG_LEVEL);

ZBUS_CHAN_DEFINE(mag_data_chan,
		 struct mag_data,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

/** Latest sample, filled and published by mag_poll(). */
static struct mag_data sample;

int mag_init(const struct device *dev)
{
	if (dev == NULL) {
		LOG_ERR("Magnetometer device is NULL");
		return -EINVAL;
	}

	if (!device_is_ready(dev)) {
		LOG_ERR("%s: device not ready", dev->name);
		return -ENODEV;
	}

	return 0;
}

/* mag_poll - see mag.h */
int mag_poll(const struct device *dev, struct mag_data *out)
{
	int ret = sensor_sample_fetch(dev);

	if (ret == -EBUSY) {
		/* A background conversion is still running */
		return -EAGAIN;
	}

	if (ret != 0) {
		LOG_ERR_RATELIMIT("Failed to fetch mag data (%d)", ret);
		return ret;
	}

	ret = sensor_channel_get(dev, SENSOR_CHAN_MAGN_XYZ, sample.field);
	if (ret != 0) {
		LOG_ERR_RATELIMIT("Failed to get mag data (%d)", ret);
		return ret;
	}

	if (out != NULL) {
		*out = sample;
	}

	ret = zbus_chan_pub(&mag_data_chan, &sample, K_NO_WAIT);
	if (ret != 0) {
		LOG_ERR_RATELIMIT("Failed to publish mag data (%d)", ret);
	}

	return ret;
}

#if defined(CONFIG_DATA_LOGGER_BIN)
void log_mag_data(const struct mag_data *mag)
{
	uint64_t ts = k_ticks_to_ns_floor64(k_uptime_ticks());
	struct datapoint dp = {
		.timestamp_ns = ts,
		.type = AURORA_DATA_IMU_MAG,
		.channel_count = 3,
		.channels = {mag->field[0], mag->field[1], mag->field[2]},
	};
	log_enqueue(&dp);
}
#endif
