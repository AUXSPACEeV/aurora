/*
 * Copyright (c) 2025, Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

#include <lib/baro.h>

LOG_MODULE_REGISTER(baro, CONFIG_AURORA_SENSORS_LOG_LEVEL);

int baro_measure(const struct device *dev, struct sensor_value *temp,
				 struct sensor_value *press)
{
	int ret;

	if (dev == NULL || (temp == NULL && press == NULL))
		return -EINVAL;

	ret = sensor_sample_fetch(dev);
	if (ret) {
		LOG_ERR("Failed to fetch baro sample (%d)\n", ret);
		return ret;
	}

	if (temp) {
		ret = sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, temp);
		if (ret) {
			LOG_ERR("Failed to get baro temperature (%d)\n", ret);
			return ret;
		}
	}

	if (press) {
		ret = sensor_channel_get(dev, SENSOR_CHAN_PRESS, press);
		if (ret) {
			LOG_ERR("Failed to get baro pressure (%d)\n", ret);
			return ret;
		}
	}

	return 0;
}

float baro_altitude(float pressure)
{
	const float P0 = 101325.0f; // standard sea-level pressure in Pa
	float altitude;

	altitude = 44330.0f * (1.0f - powf(pressure / P0, 0.1903f));

	return altitude;
}