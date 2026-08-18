/**
 * @file baro.c
 * @brief Barometric pressure sensor library implementation.
 *
 * Supplies one sample source, selected at build time: the hardware driver
 * (polled, or paced by its data-ready trigger), the synthetic flight profile,
 * or a recorded flight played back. All of them publish struct baro_data on
 * baro_data_chan, so everything downstream is unaware of which is in use.
 *
 * Also holds the pressure-to-altitude conversion used by the state machine.
 *
 * Copyright (c) 2025-2026, Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/zbus/zbus.h>

#include <aurora/lib/baro.h>
#include <aurora/lib/data_logger.h>

#if defined(CONFIG_AURORA_FAKE_SENSORS)
#include <aurora/lib/sim.h>
#endif /* CONFIG_AURORA_FAKE_SENSORS */

#if defined(CONFIG_AURORA_FAKE_SENSORS_REPLAY)
#include "replay.h"
#endif /* CONFIG_AURORA_FAKE_SENSORS_REPLAY */

LOG_MODULE_REGISTER(baro, CONFIG_AURORA_SENSORS_LOG_LEVEL);

ZBUS_CHAN_DEFINE(baro_data_chan,
		 struct baro_data,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

/** Latest sample, filled by the active source and published by baro_measure(). */
static struct baro_data sample;

#if defined(CONFIG_AURORA_FAKE_SENSORS_REPLAY)

/* -------- Recorded-flight playback -------- */

/** Where playback has got to; see the IMU cursor in imu.c. */
static struct {
	uint64_t origin;
	size_t idx;
	bool end_logged;
} cursor;

static size_t replay_seek(size_t idx, uint64_t t_ns,
			  const struct replay_baro_sample *track, size_t len)
{
	while (idx + 1 < len && track[idx + 1].t_ns <= t_ns) {
		idx++;
	}
	return idx;
}

int baro_init(const struct device *dev)
{
	ARG_UNUSED(dev);

	if (replay_baro_len == 0) {
		LOG_ERR("Replay baro: recording holds no samples");
		return -ENODATA;
	}

	LOG_INF("Replay baro: %zu samples", replay_baro_len);
	return 0;
}

static int baro_acquire(const struct device *dev)
{
	ARG_UNUSED(dev);

	const uint64_t origin = sim_launch_origin();

	if (origin != cursor.origin) {
		cursor.origin = origin;
		cursor.idx = 0;
		cursor.end_logged = false;
	}

	const struct replay_baro_sample *b = &replay_baro[0];

	if (origin != 0) {
		const uint64_t now = k_ticks_to_ns_floor64(k_uptime_ticks());
		const uint64_t t_ns = (now > origin) ? (now - origin) : 0;

		cursor.idx = replay_seek(cursor.idx, t_ns, replay_baro,
					 replay_baro_len);

		if (!cursor.end_logged &&
		    t_ns > replay_baro[replay_baro_len - 1].t_ns) {
			LOG_INF("Replay baro: end of recording, holding final sample");
			cursor.end_logged = true;
		}

		b = &replay_baro[cursor.idx];
	}

	sim_set_sensor_value(&sample.temperature, b->temp_c);
	sim_set_sensor_value(&sample.pressure, b->pres_kpa);

	return 0;
}

#elif defined(CONFIG_AURORA_FAKE_SENSORS)

/* -------- Synthetic flight profile -------- */

int baro_init(const struct device *dev)
{
	ARG_UNUSED(dev);

	LOG_INF("Simulated baro ready, sampled at %d Hz", CONFIG_BARO_FREQUENCY);
	return 0;
}

static int baro_acquire(const struct device *dev)
{
	ARG_UNUSED(dev);

	double altitude, accel_vert;

	sim_profile_sample(sim_flight_time_s(), &altitude, &accel_vert);

	sim_set_sensor_value(&sample.temperature, 20.0);
	sim_set_sensor_value(&sample.pressure,
			     sim_altitude_to_pressure_kpa(altitude));

	return 0;
}

#else

/* -------- Hardware driver -------- */

#if defined(CONFIG_BARO_TRIGGER)
/** Set by the trigger handler, consumed by baro_acquire(). */
static atomic_t drdy;
/** False if the driver refused the trigger and we fell back to polling. */
static bool triggered;

/**
 * @brief Data-ready handler.
 *
 * Reads the waiting sample and raises a flag; everything downstream runs on
 * the state machine thread. See the IMU handler in imu.c for why the read
 * has to happen here rather than being deferred with the rest.
 */
static void baro_trigger_handler(const struct device *dev,
				 const struct sensor_trigger *trigger)
{
	ARG_UNUSED(trigger);

	int ret = sensor_sample_fetch_chan(dev, SENSOR_CHAN_ALL);

	if (ret != 0) {
		LOG_ERR_RATELIMIT("Failed to fetch baro data (%d)", ret);
		return;
	}

	atomic_set(&drdy, 1);
}
#endif /* CONFIG_BARO_TRIGGER */

int baro_init(const struct device *dev)
{
	if (dev == NULL) {
		LOG_ERR("Baro device is NULL");
		return -EINVAL;
	}

	if (!device_is_ready(dev)) {
		LOG_ERR("%s: device not ready", dev->name);
		return -ENODEV;
	}

#if defined(CONFIG_BARO_TRIGGER)
	/* Not every barometer driver offers one. The BMP581 exposes its
	 * interrupt only through the asynchronous RTIO streaming API and has
	 * no trigger_set at all, so this reports -ENOTSUP there and the caller
	 * polls instead.
	 */
	static const struct sensor_trigger trig = {
		.type = SENSOR_TRIG_DATA_READY,
		.chan = SENSOR_CHAN_ALL,
	};

	if (sensor_trigger_set(dev, &trig, baro_trigger_handler) != 0) {
		LOG_WRN("%s: no data-ready trigger available", dev->name);
		return -ENOTSUP;
	}

	triggered = true;
#endif /* CONFIG_BARO_TRIGGER */

	return 0;
}

static int baro_acquire(const struct device *dev)
{
	int ret;

#if defined(CONFIG_BARO_TRIGGER)
	if (triggered) {
		/* The handler already ran the bus transfer. */
		if (!atomic_cas(&drdy, 1, 0)) {
			return -EAGAIN;
		}
	} else
#endif /* CONFIG_BARO_TRIGGER */
	{
		ret = sensor_sample_fetch(dev);
		if (ret != 0) {
			LOG_ERR_RATELIMIT("Failed to fetch baro data (%d)", ret);
			return ret;
		}
	}

	/* Held against the driver's cooperative trigger thread, so a fetch
	 * cannot land between the two reads and splice two samples together.
	 */
	k_sched_lock();
	ret = sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, &sample.temperature);
	if (ret == 0) {
		ret = sensor_channel_get(dev, SENSOR_CHAN_PRESS, &sample.pressure);
	}
	k_sched_unlock();

	if (ret != 0) {
		LOG_ERR_RATELIMIT("Failed to get baro data (%d)", ret);
	}

	return ret;
}

#endif /* sample source */

/* baro_measure - see baro.h */
int baro_measure(const struct device *dev, struct baro_data *out)
{
	int ret = baro_acquire(dev);

	if (ret != 0) {
		return ret;
	}

	if (out != NULL) {
		*out = sample;
	}

	ret = zbus_chan_pub(&baro_data_chan, &sample, K_NO_WAIT);
	if (ret != 0) {
		LOG_ERR_RATELIMIT("Failed to publish baro data (%d)", ret);
	}

	return ret;
}

/*-----------------------------------------------------------
 * Pressure-to-altitude conversion (ISA troposphere)
 *----------------------------------------------------------*/

/** ISA sea-level temperature (K). */
#define ISA_T0 288.15

/** ISA temperature lapse rate (K/m). */
#define ISA_L  0.0065

/** R·L / (g·M) exponent for the hypsometric formula. */
#define ISA_RL_OVER_GM 0.190263

/** Ground-level reference pressure in kPa (0 = not set). */
static double ref_pressure_kpa;

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

	if (!isfinite(press_kpa)) {
		LOG_WRN_RATELIMIT("implausible pressure %.3f kPa; sample dropped",
				  press_kpa);
		return -EDOM;
	}

	if (baro_set_reference(press_kpa) != 0) {
		return -EINVAL;
	}

	double altitude = baro_pressure_to_altitude(press_kpa);

	if (!isfinite(altitude)) {
		LOG_WRN_RATELIMIT("non-finite altitude from %.3f kPa; sample dropped",
				  press_kpa);
		return -EDOM;
	}

	*altitude_out = altitude;
	return 0;
}

#if defined(CONFIG_DATA_LOGGER_BIN)
void log_baro_data(const struct baro_data *baro)
{
	uint64_t ts = k_ticks_to_ns_floor64(k_uptime_ticks());
	struct datapoint dp = {
		.timestamp_ns = ts,
		.type = AURORA_DATA_BARO,
		.channel_count = 2,
		.channels = {baro->temperature, baro->pressure},
	};
	log_enqueue(&dp);
}
#endif
