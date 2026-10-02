/**
 * @file haccel.c
 * @brief High-g accelerometer library implementation.
 *
 * Polls the hardware driver and publishes struct haccel_data on
 * haccel_data_chan. There is no simulated source; see HACCEL in Kconfig.
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
#include <aurora/lib/haccel.h>

LOG_MODULE_REGISTER(haccel, CONFIG_AURORA_SENSORS_LOG_LEVEL);

ZBUS_CHAN_DEFINE(haccel_data_chan,
		 struct haccel_data,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

/** Latest sample, filled and published by haccel_poll(). */
static struct haccel_data sample;

int haccel_init(const struct device *dev)
{
	if (dev == NULL) {
		LOG_ERR("High-g accelerometer device is NULL");
		return -EINVAL;
	}

	if (!device_is_ready(dev)) {
		LOG_ERR("%s: device not ready", dev->name);
		return -ENODEV;
	}

	return 0;
}

/* haccel_poll - see haccel.h */
int haccel_poll(const struct device *dev, struct haccel_data *out)
{
	int ret = sensor_sample_fetch(dev);

	if (ret != 0) {
		LOG_ERR_RATELIMIT("Failed to fetch high-g accel data (%d)", ret);
		return ret;
	}

	ret = sensor_channel_get(dev, SENSOR_CHAN_ACCEL_XYZ, sample.accel);
	if (ret != 0) {
		LOG_ERR_RATELIMIT("Failed to get high-g accel data (%d)", ret);
		return ret;
	}

	if (out != NULL) {
		*out = sample;
	}

	ret = zbus_chan_pub(&haccel_data_chan, &sample, K_NO_WAIT);
	if (ret != 0) {
		LOG_ERR_RATELIMIT("Failed to publish high-g accel data (%d)", ret);
	}

	return ret;
}

#if defined(CONFIG_DATA_LOGGER_BIN)
void log_haccel_data(const struct haccel_data *haccel)
{
	uint64_t ts = k_ticks_to_ns_floor64(k_uptime_ticks());
	struct datapoint dp = {
		.timestamp_ns = ts,
		.type = AURORA_DATA_HACCEL,
		.channel_count = 3,
		.channels = {haccel->accel[0], haccel->accel[1], haccel->accel[2]},
	};
	log_enqueue(&dp);
}
#endif
