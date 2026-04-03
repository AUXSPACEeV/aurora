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
#include <zephyr/zbus/zbus.h>

#include <aurora/lib/baro.h>

LOG_MODULE_REGISTER(baro, CONFIG_AURORA_SENSORS_LOG_LEVEL);

ZBUS_CHAN_DEFINE(baro_data_chan,
				 baro_data_t,
				 NULL,
				 NULL,
				 ZBUS_OBSERVERS_EMPTY,
				 ZBUS_MSG_INIT(0));


/**
 * @brief Fetch and log accelerometer and gyroscope readings.
 *
 * @param dev Pointer to the IMU device.
 *
 * @return 0 on success, -errno on failure.
 */
static int fetch_and_send(const struct device *dev)
{
	baro_data_t msg;
	int ret;

	ret = sensor_sample_fetch(dev);
	if ( ret != 0) {
		LOG_ERR("Failed to fetch sensor data\n");
		return ret;
	}

	ret = sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, &(msg.temperature));
	if (ret != 0) {
		LOG_ERR("Failed to get baro temperature\n");
		return ret;
	}

	ret = sensor_channel_get(dev, SENSOR_CHAN_PRESS, &(msg.pressure));
	if (ret != 0) {
		LOG_ERR("Failed to get baro pressure\n");
		return ret;
	}

	/* Publish the baro data to the z-bus channel */
	ret = zbus_chan_pub(&baro_data_chan, &msg, K_NO_WAIT);
	if (ret != 0) {
		LOG_ERR("Failed to publish baro data\n");
	}
	return ret;
}


#if defined(CONFIG_LPS22HH_TRIGGER)
/**
 * @brief Data-ready trigger callback for the baro.
 *
 * @param dev  Pointer to the baro device.
 * @param trig Pointer to the sensor trigger descriptor.
 */
static void trigger_handler(const struct device *dev,
							const struct sensor_trigger *trig)
{
	fetch_and_send(dev);
}

/**
 * @brief Configure the baro to run in data-ready trigger mode.
 *
 * @param dev Pointer to the baro device.
 */
static void run_trigger_mode(const struct device *dev)
{
	struct sensor_trigger trig;

	trig.type = SENSOR_TRIG_DATA_READY;
	trig.chan = SENSOR_CHAN_ALL;

	if (sensor_trigger_set(dev, &trig, trigger_handler) != 0) {
		LOG_ERR("Could not set sensor type and channel\n");
		return;
	}
}
#else
/* baro_measure – see baro.h */
int baro_measure(const struct device *dev)
{
	if (dev == NULL)
		return -EINVAL;

	return fetch_and_send(dev);
}
#endif /* CONFIG_LPS22HH_TRIGGER */

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

	#if defined(CONFIG_LPS22HH_TRIGGER)
	run_trigger_mode(dev);
	LOG_DBG("Baro initialized in trigger mode");
	#endif /* CONFIG_LPS22HH_TRIGGER */

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
