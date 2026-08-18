/**
 * @file imu.c
 * @brief IMU sensor library implementation.
 *
 * Supplies one sample source, selected at build time: the hardware driver
 * (polled, or paced by its data-ready trigger), the synthetic flight profile,
 * or a recorded flight played back. All of them publish struct imu_data on
 * imu_data_chan, so everything downstream is unaware of which is in use.
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

#include <aurora/lib/data_logger.h>
#include <aurora/lib/imu.h>

#if defined(CONFIG_AURORA_FAKE_SENSORS)
#include <aurora/lib/sim.h>
#endif /* CONFIG_AURORA_FAKE_SENSORS */

#if defined(CONFIG_AURORA_FAKE_SENSORS_REPLAY)
#include "replay.h"
#endif /* CONFIG_AURORA_FAKE_SENSORS_REPLAY */

#ifndef M_PI
#define M_PI ((double)3.1415926535)
#endif

LOG_MODULE_REGISTER(imu, CONFIG_AURORA_SENSORS_LOG_LEVEL);

ZBUS_CHAN_DEFINE(imu_data_chan,
		 struct imu_data,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));

/** Latest sample, filled by the active source and published by imu_poll(). */
static struct imu_data sample;

static inline double out_ev(const struct sensor_value *val)
{
	return (val->val1 + (double)val->val2 / 1000000);
}

#if defined(CONFIG_AURORA_FAKE_SENSORS_REPLAY)

/* -------- Recorded-flight playback -------- */

/**
 * @brief Where playback has got to.
 *
 * The indices only move forward within a run; a launch or reset changes the
 * origin, which is the cue to rewind.
 */
static struct {
	uint64_t origin;
	size_t accel_idx;
	size_t gyro_idx;
	bool end_logged;
} cursor;

/**
 * @brief Advance to the newest sample at or before @p t_ns.
 *
 * Reading a sensor returns whatever it holds at that instant, so samples the
 * caller slept past are skipped rather than queued: the poll rate decides how
 * much of the recording is seen.
 */
static size_t replay_seek(size_t idx, uint64_t t_ns,
			  const struct replay_imu_sample *track, size_t len)
{
	while (idx + 1 < len && track[idx + 1].t_ns <= t_ns) {
		idx++;
	}
	return idx;
}

int imu_init(const struct device *dev)
{
	ARG_UNUSED(dev);

	if (replay_accel_len == 0 || replay_gyro_len == 0) {
		LOG_ERR("Replay IMU: recording holds no samples");
		return -ENODATA;
	}

	LOG_INF("Replay IMU: %zu accel + %zu gyro samples", replay_accel_len,
		replay_gyro_len);
	return 0;
}

static int imu_acquire(const struct device *dev)
{
	ARG_UNUSED(dev);

	const uint64_t origin = sim_launch_origin();

	if (origin != cursor.origin) {
		cursor.origin = origin;
		cursor.accel_idx = 0;
		cursor.gyro_idx = 0;
		cursor.end_logged = false;
	}

	/* Pad-stationary holds the first recorded sample, so attitude
	 * calibration sees a valid stationary signal.
	 */
	const struct replay_imu_sample *a = &replay_accel[0];
	const struct replay_imu_sample *g = &replay_gyro[0];

	if (origin != 0) {
		const uint64_t now = k_ticks_to_ns_floor64(k_uptime_ticks());
		const uint64_t t_ns = (now > origin) ? (now - origin) : 0;

		cursor.accel_idx = replay_seek(cursor.accel_idx, t_ns,
					       replay_accel, replay_accel_len);
		cursor.gyro_idx = replay_seek(cursor.gyro_idx, t_ns,
					      replay_gyro, replay_gyro_len);

		if (!cursor.end_logged &&
		    t_ns > replay_accel[replay_accel_len - 1].t_ns) {
			LOG_INF("Replay IMU: end of recording, holding final sample");
			cursor.end_logged = true;
		}

		a = &replay_accel[cursor.accel_idx];
		g = &replay_gyro[cursor.gyro_idx];
	}

	sim_set_sensor_value(&sample.accel[0], a->x);
	sim_set_sensor_value(&sample.accel[1], a->y);
	sim_set_sensor_value(&sample.accel[2], a->z);
	sim_set_sensor_value(&sample.gyro[0], g->x);
	sim_set_sensor_value(&sample.gyro[1], g->y);
	sim_set_sensor_value(&sample.gyro[2], g->z);

	return 0;
}

#elif defined(CONFIG_AURORA_FAKE_SENSORS)

/* -------- Synthetic flight profile -------- */

int imu_init(const struct device *dev)
{
	ARG_UNUSED(dev);

	LOG_INF("Simulated IMU ready, sampled at %d Hz", CONFIG_IMU_FREQUENCY);
	return 0;
}

static int imu_acquire(const struct device *dev)
{
	ARG_UNUSED(dev);

	const double t_s = sim_flight_time_s();
	double altitude, accel_vert;

	sim_profile_sample(t_s, &altitude, &accel_vert);

	double accel[IMU_NUM_AXES] = {0.0, 0.0, 0.0};
	double gyro[IMU_NUM_AXES] = {0.0, 0.0, 0.0};

	accel[CONFIG_IMU_UP_AXIS_INDEX] = accel_vert * CONFIG_IMU_UP_AXIS_SIGN;

	/* Roll about the up axis, but only once airborne: a stationary gyro
	 * pre-launch is what lets attitude calibration capture a zero bias.
	 */
	if (t_s >= 0.0) {
		gyro[CONFIG_IMU_UP_AXIS_INDEX] = SIM_ROLL_RATE_DPS *
						 (M_PI / 180.0) *
						 CONFIG_IMU_UP_AXIS_SIGN;
	}

	for (int i = 0; i < IMU_NUM_AXES; i++) {
		sim_set_sensor_value(&sample.accel[i], accel[i]);
		sim_set_sensor_value(&sample.gyro[i], gyro[i]);
	}

	return 0;
}

#else

/* -------- Hardware driver -------- */

#if defined(CONFIG_IMU_TRIGGER)
/** Set by the trigger handler, consumed by imu_acquire(). */
static atomic_t drdy;
/** False if the driver refused the trigger and we fell back to polling. */
static bool triggered;

/**
 * @brief Data-ready handler.
 *
 * Reads the waiting sample and raises a flag. Nothing else: the conversion,
 * the publish and every consumer downstream run on the state machine thread,
 * off this driver-owned cooperative one.
 *
 * The read itself is not optional. lsm6dso_handle_interrupt() loops on
 * STATUS_REG until both XLDA and GDA are clear, and only reading the output
 * registers clears them, so a handler that raised the flag and returned would
 * spin that thread forever -- at a cooperative priority nothing below can
 * preempt. SENSOR_CHAN_ALL is what clears both; fetching only
 * SENSOR_CHAN_ACCEL_XYZ leaves GDA set and hangs just the same.
 */
static void imu_trigger_handler(const struct device *dev,
				const struct sensor_trigger *trigger)
{
	ARG_UNUSED(trigger);

	int ret = sensor_sample_fetch_chan(dev, SENSOR_CHAN_ALL);

	if (ret != 0) {
		LOG_ERR_RATELIMIT("Failed to fetch IMU data (%d)", ret);
		return;
	}

	atomic_set(&drdy, 1);
}
#endif /* CONFIG_IMU_TRIGGER */

int imu_init(const struct device *dev)
{
	if (dev == NULL) {
		LOG_ERR("IMU device is NULL");
		return -EINVAL;
	}

	if (!device_is_ready(dev)) {
		LOG_ERR("%s: device not ready", dev->name);
		return -ENODEV;
	}

#if defined(CONFIG_IMU_TRIGGER)
	/* Only the accelerometer's line is subscribed to, deliberately: the
	 * handler reads accelerometer and gyroscope together and both run at
	 * the same ODR, so a second trigger would double the interrupt load to
	 * deliver the same samples. The channel must be named -- the LSM6DSO
	 * family rejects SENSOR_CHAN_ALL here with -ENOTSUP.
	 */
	static const struct sensor_trigger trig = {
		.type = SENSOR_TRIG_DATA_READY,
		.chan = SENSOR_CHAN_ACCEL_XYZ,
	};

	if (sensor_trigger_set(dev, &trig, imu_trigger_handler) != 0) {
		LOG_WRN("%s: no data-ready trigger available", dev->name);
		return -ENOTSUP;
	}

	triggered = true;
#endif /* CONFIG_IMU_TRIGGER */

	return 0;
}

static int imu_acquire(const struct device *dev)
{
	int ret;

#if defined(CONFIG_IMU_TRIGGER)
	if (triggered) {
		/* The handler already ran the bus transfer. */
		if (!atomic_cas(&drdy, 1, 0)) {
			return -EAGAIN;
		}
	} else
#endif /* CONFIG_IMU_TRIGGER */
	{
		ret = sensor_sample_fetch(dev);
		if (ret != 0) {
			LOG_ERR_RATELIMIT("Failed to fetch IMU data (%d)", ret);
			return ret;
		}
	}

	/* The trigger thread is cooperative and outranks this one, so a fetch
	 * landing between the two reads below would splice two samples
	 * together. Neither read touches the bus, so holding the scheduler
	 * across them costs nothing.
	 */
	k_sched_lock();
	ret = sensor_channel_get(dev, SENSOR_CHAN_ACCEL_XYZ, sample.accel);
	if (ret == 0) {
		ret = sensor_channel_get(dev, SENSOR_CHAN_GYRO_XYZ, sample.gyro);
	}
	k_sched_unlock();

	if (ret != 0) {
		LOG_ERR_RATELIMIT("Failed to get IMU data (%d)", ret);
	}

	return ret;
}

#endif /* sample source */

/* imu_poll - see imu.h */
int imu_poll(const struct device *dev, struct imu_data *out)
{
	int ret = imu_acquire(dev);

	if (ret != 0) {
		return ret;
	}

	if (out != NULL) {
		*out = sample;
	}

	ret = zbus_chan_pub(&imu_data_chan, &sample, K_NO_WAIT);
	if (ret != 0) {
		LOG_ERR_RATELIMIT("Failed to publish IMU data (%d)", ret);
	}

	return ret;
}

/* imu_sensor_value_to_acceleration - see imu.h */
int imu_sensor_value_to_acceleration(const struct imu_data *data, double *acc_out)
{
	if (data == NULL || acc_out == NULL)
		return -EINVAL;

	double x = out_ev(&data->accel[0]);
	double y = out_ev(&data->accel[1]);
	double z = out_ev(&data->accel[2]);
	*acc_out = sqrt(x*x + y*y + z*z);
	return 0;
}

/* imu_sensor_value_to_orientation - see imu.h */
int imu_sensor_value_to_orientation(const struct imu_data *data,
				    double dt_s,
				    const double gyro_bias[IMU_NUM_AXES],
				    double *orientation)
{
	if (data == NULL || orientation == NULL)
		return -EINVAL;

	const int idx = CONFIG_IMU_UP_AXIS_INDEX;
	const int sign = CONFIG_IMU_UP_AXIS_SIGN;

	double a[3] = {
		out_ev(&data->accel[0]),
		out_ev(&data->accel[1]),
		out_ev(&data->accel[2]),
	};

	/* Re-map body axes so the configured "up" axis becomes Z-up.  The two
	 * remaining axes are taken in cyclic order as the local X (forward)
	 * and Y (lateral) axes.
	 */
	const int x_idx = (idx + 1) % 3;
	const int y_idx = (idx + 2) % 3;

	const double gx = a[x_idx];
	const double gy = a[y_idx];
	const double gz = (double)sign * a[idx];

	const double rad2deg = 180.0 / M_PI;

	orientation[0] = atan2(gy, gz) * rad2deg;
	orientation[1] = atan2(-gx, sqrt(gy * gy + gz * gz)) * rad2deg;

	/* Roll is rotation about the up (long) axis, unobservable from a
	 * static accelerometer reading.  Integrate the gyro component along
	 * the up axis (with mounting sign) into orientation[2].
	 */
	if (dt_s > 0.0) {
		double w_up = out_ev(&data->gyro[idx]);
		if (gyro_bias != NULL) {
			w_up -= gyro_bias[idx];
		}
		w_up *= (double)sign;
		double roll = orientation[2] + w_up * dt_s * rad2deg;
		/* Wrap to [-180, 180]. */
		roll = fmod(roll + 180.0, 360.0);
		if (roll < 0.0) {
			roll += 360.0;
		}
		orientation[2] = roll - 180.0;
	}

	return 0;
}

#if defined(CONFIG_DATA_LOGGER_BIN)
void log_imu_data(const struct imu_data *imu)
{
	uint64_t ts = k_ticks_to_ns_floor64(k_uptime_ticks());
	struct datapoint dp = {
		.timestamp_ns = ts,
		.type = AURORA_DATA_IMU_ACCEL,
		.channel_count = 3,
		.channels = {imu->accel[0], imu->accel[1], imu->accel[2]},
	};
	log_enqueue(&dp);

	dp.type = AURORA_DATA_IMU_GYRO;
	dp.channels[0] = imu->gyro[0];
	dp.channels[1] = imu->gyro[1];
	dp.channels[2] = imu->gyro[2];
	log_enqueue(&dp);
}
#endif
