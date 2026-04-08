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
		 struct baro_data,
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
	struct baro_data msg;
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


#if defined(CONFIG_BARO_TRIGGER)
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
#endif /* CONFIG_BARO_TRIGGER */

/* baro_init – see baro.h */
int baro_init(const struct device *dev)
{
	if (dev == NULL) {
		LOG_ERR("Baro device is NULL");
		return -ENODEV;
	}
	if (!device_is_ready(dev)) {
		LOG_ERR("Baro device %s is not ready", dev->name);
		return -ETIMEDOUT;
	}

#if defined(CONFIG_BARO_TRIGGER)
	run_trigger_mode(dev);
	LOG_DBG("Baro initialized in trigger mode");
#endif /* CONFIG_BARO_TRIGGER */

	return 0;
}

/*-----------------------------------------------------------
 * Pressure-to-altitude conversion (ISA troposphere)
 *----------------------------------------------------------*/

/** ISA sea-level temperature (K). */
#define ISA_T0 288.15

/** ISA temperature lapse rate (K/m). */
#define ISA_L  0.0065

/** g·M / (R·L) exponent for the barometric formula. */
#define ISA_GMR_OVER_L 5.25588

/** R·L / (g·M) exponent for the hypsometric formula. */
#define ISA_RL_OVER_GM 0.190263

/** Ground-level reference pressure in kPa (0 = not set). */
static double ref_pressure_kpa;

/* baro_pressure_to_altitude – see baro.h */
static double baro_pressure_to_altitude(double press_kpa)
{
	/*
	 * Hypsometric formula (ISA troposphere):
	 *   h = (T0 / L) * (1 - (P / P_ref) ^ (R·L / (g·M)))
	 */
	return (ISA_T0 / ISA_L) *
	       (1.0 - pow(press_kpa / ref_pressure_kpa, ISA_RL_OVER_GM));
}

/* baro_set_reference – see baro.h */
int baro_set_reference(double ref_kpa)
{
	static bool ref_set = false;

	if (ref_kpa <= 0.0)
		return -EINVAL;

	if (!ref_set)
	{
		ref_pressure_kpa = ref_kpa;
		ref_set = true;
	}

	/* Success even if reference is already set */
	return 0;
}

/* baro_sensor_value_to_altitude – see baro.h */
int baro_sensor_value_to_altitude(const struct sensor_value *press, double *altitude_out)
{
	if (press == NULL || altitude_out == NULL)
		return -EINVAL;

	double press_kpa = (double)press->val1 + (double)press->val2 / 1e6;

	if (baro_set_reference(press_kpa) != 0) {
		return -EINVAL;
	}

	*altitude_out = baro_pressure_to_altitude(press_kpa);
	return 0;
}
