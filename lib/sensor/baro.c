/**
 * @file baro.c
 * @brief Barometric pressure sensor library implementation.
 *
 * Wraps the Zephyr sensor API for the MS5607 barometric sensor, providing
 * measurement, altitude computation, and initialization helpers.
 *
 * Copyright (c) 2025-2026, Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

#include <aurora/lib/baro.h>

LOG_MODULE_REGISTER(baro, CONFIG_AURORA_SENSORS_LOG_LEVEL);

/* baro_measure – see baro.h */
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

/**
 * @brief Set the barometric sensor oversampling rate.
 *
 * @param dev Pointer to the barometric sensor device.
 * @param osr Oversampling rate value.
 * @return 0 on success, -EIO on failure.
 */
int baro_set_oversampling(const struct device *dev, uint32_t osr)
{
	struct sensor_value oversampling_rate = { osr, 0 };

	if (sensor_attr_set(dev, SENSOR_CHAN_ALL, SENSOR_ATTR_OVERSAMPLING,
						&oversampling_rate) != 0) {
		LOG_ERR("Could not set oversampling rate of %d "
				"on Baro device, aborting test.",
				oversampling_rate.val1);
		return -EIO;
	}
	return 0;
}

/* baro_init – see baro.h */
int baro_init(const struct device *dev)
{
	if (!device_is_ready(dev)) {
		LOG_ERR("Baro device %s is not ready, aborting test.",
				dev->name);
		return -ETIMEDOUT;
	}

	return baro_set_oversampling(dev, CONFIG_BARO_OVERSAMPLING_VALUE);
}

/*-----------------------------------------------------------
 * Pressure-to-altitude conversion (ISA troposphere)
 *----------------------------------------------------------*/

/** ISA sea-level temperature (K). */
#define ISA_T0 288.15f

/** ISA temperature lapse rate (K/m). */
#define ISA_L  0.0065f

/** g·M / (R·L) exponent for the barometric formula. */
#define ISA_GMR_OVER_L 5.25588f

/** R·L / (g·M) exponent for the hypsometric formula. */
#define ISA_RL_OVER_GM 0.190263f

/** Ground-level reference pressure in kPa (0 = not set). */
static float ref_pressure_kpa;

/* baro_set_reference – see baro.h */
int baro_set_reference(float ref_kpa)
{
	if (ref_kpa <= 0.0f)
		return -EINVAL;

	ref_pressure_kpa = ref_kpa;
	return 0;
}

/* baro_pressure_to_altitude – see baro.h */
float baro_pressure_to_altitude(float press_kpa)
{
	/*
	 * Hypsometric formula (ISA troposphere):
	 *   h = (T0 / L) * (1 - (P / P_ref) ^ (R·L / (g·M)))
	 */
	return (ISA_T0 / ISA_L) *
	       (1.0f - powf(press_kpa / ref_pressure_kpa, ISA_RL_OVER_GM));
}
