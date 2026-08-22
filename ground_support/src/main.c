/**
 * @file main.c
 * @brief Ground Support application entry point.
 *
 * A pad-link and LoRa/HC-12 ground station. One thread owns the onboard
 * sensors: it reads the IMU and barometer as they come due, derives the
 * unit's own attitude (roll/pitch), tilt-compensated magnetic heading and
 * barometric altitude, and publishes them to the shared snapshot. The GNSS
 * callback, the HC-12 receiver and the LVGL display run on their own threads
 * and rendezvous through the same snapshot (see ground_state.h).
 *
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ground_state.h"
#include "hc12_rx.h"

#if defined(CONFIG_GNSS)
#include "gnss_rx.h"
#endif /* CONFIG_GNSS */

#if defined(CONFIG_LVGL)
#include "display.h"
#endif /* CONFIG_LVGL */

#include <math.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zephyr/app_version.h>

#if defined(CONFIG_IMU)
#include <aurora/lib/imu.h>
#endif /* CONFIG_IMU */

#if defined(CONFIG_BARO)
#include <aurora/lib/baro.h>
#endif /* CONFIG_BARO */

LOG_MODULE_REGISTER(main, CONFIG_GROUND_SUPPORT_LOG_LEVEL);

#define STANDARD_GRAVITY 9.80665

/* ============================================================
 *                     SENSORS
 * ============================================================ */

/* Both sensors share I2C0 and are read from the sampler thread below, one at
 * a time; owning the bus from a single thread avoids the spin-serialisation a
 * shared I2C controller would otherwise impose on two concurrent pollers.
 */

/** Sleep cap when no sensor has a deadline, e.g. every one failed to init. */
#define SENSOR_IDLE_SLEEP_MS 1000

#if defined(CONFIG_IMU)
#define IMU_PERIOD_MS MAX(1, 1000 / CONFIG_IMU_FREQUENCY)

#if !DT_HAS_CHOSEN(auxspace_imu)
#error "CONFIG_IMU requires DT chosen 'auxspace,imu' to point at an IMU sensor node."
#endif
BUILD_ASSERT(DT_NODE_HAS_STATUS(DT_CHOSEN(auxspace_imu), okay),
	     "the 'auxspace,imu' chosen node must have status \"okay\"");
#define IMU_DEV DEVICE_DT_GET(DT_CHOSEN(auxspace_imu))

static bool imu_ok;
#endif /* CONFIG_IMU */

#if defined(CONFIG_BARO)
#define BARO_PERIOD_MS MAX(1, 1000 / CONFIG_BARO_FREQUENCY)

#if !DT_HAS_CHOSEN(auxspace_baro)
#error "CONFIG_BARO requires DT chosen 'auxspace,baro' to point at a baro sensor node."
#endif
BUILD_ASSERT(DT_NODE_HAS_STATUS(DT_CHOSEN(auxspace_baro), okay),
	     "the 'auxspace,baro' chosen node must have status \"okay\"");
#define BARO_DEV DEVICE_DT_GET(DT_CHOSEN(auxspace_baro))

static bool baro_ok;
static bool baro_ref_set;
#endif /* CONFIG_BARO */

/**
 * @brief Bring every configured sensor up.
 *
 * A sensor that fails to initialize is left out rather than taking the others
 * down with it; one that comes up but cannot deliver its data-ready trigger
 * falls back to being polled.
 */
static void sensors_init(void)
{
#if defined(CONFIG_IMU)
	int imu_rc = imu_init(IMU_DEV);

	if (imu_rc == -ENOTSUP) {
		LOG_WRN("IMU data-ready trigger unavailable; polling at %d Hz",
			CONFIG_IMU_FREQUENCY);
		imu_rc = 0;
	}

	imu_ok = (imu_rc == 0);
	if (imu_ok) {
		LOG_INF("IMU ready");
	} else {
		LOG_ERR("IMU not ready!");
	}
#endif /* CONFIG_IMU */

#if defined(CONFIG_BARO)
	int baro_rc = baro_init(BARO_DEV);

	if (baro_rc == -ENOTSUP) {
		LOG_WRN("Baro data-ready trigger unavailable; polling at %d Hz",
			CONFIG_BARO_FREQUENCY);
		baro_rc = 0;
	}

	baro_ok = (baro_rc == 0);
	if (baro_ok) {
		LOG_INF("Baro ready");
	} else {
		LOG_ERR("Baro not ready!");
	}
#endif /* CONFIG_BARO */
}

/* ============================================================
 *                     SAMPLER TASK
 * ============================================================ */

/* Onboard-attitude snapshot, rebuilt each pass and published whole. File-scope
 * because exactly one thread owns it.
 */
static struct gs_local local;

#if defined(CONFIG_IMU)
/* Previous IMU timestamp, for the roll integration dt. */
static int64_t last_imu_ns;

/**
 * @brief Turn one IMU sample into roll, pitch, heading and |a|.
 *
 * Attitude comes straight from the accelerometer (gravity direction) and the
 * magnetometer (heading); no calibration/gravity-tracking state machine is
 * needed here because the ground unit only ever reports its own static
 * pointing, never gravity-removed flight acceleration. The orientation triple
 * follows the imu lib convention: [0] = bank about the forward axis, [1] =
 * elevation of the forward axis, [2] = gyro-integrated roll about the up axis.
 */
static void handle_imu(const struct imu_data *imu)
{
	int64_t now_ns = (k_uptime_ticks() * NSEC_PER_SEC) / CONFIG_SYS_CLOCK_TICKS_PER_SEC;
	double dt_s = (last_imu_ns != 0) ? (double)(now_ns - last_imu_ns) / 1e9 : 0.0;

	if (dt_s < 0.0 || dt_s > 1.0) {
		dt_s = 0.0;
	}

	double orientation[3] = {0.0, 0.0, 0.0};

	if (imu_sensor_value_to_orientation(imu, dt_s, NULL, orientation) == 0) {
		local.roll_deg = orientation[0];
		local.pitch_deg = orientation[1];
	}

#if defined(CONFIG_IMU_MAGNETOMETER)
	double heading;

	if (imu_sensor_value_to_heading(imu, &heading) == 0) {
		local.heading_deg = heading;
	}
#endif /* CONFIG_IMU_MAGNETOMETER */

	double accel_ms2;

	if (imu_sensor_value_to_acceleration(imu, &accel_ms2) == 0) {
		local.accel_g = accel_ms2 / STANDARD_GRAVITY;
	}

	local.imu_ok = true;
	last_imu_ns = now_ns;
}
#endif /* CONFIG_IMU */

#if defined(CONFIG_BARO)
/**
 * @brief Turn one baro sample into altitude above the boot reference.
 *
 * The first valid pressure reading sets the zero-altitude datum, so the
 * displayed altitude is height above wherever the unit was switched on.
 */
static void handle_baro(const struct baro_data *baro)
{
	if (!baro_ref_set) {
		double ref_kpa = sensor_value_to_double(&baro->pressure);

		if (baro_set_reference(ref_kpa) == 0) {
			baro_ref_set = true;
		}
	}

	double altitude;

	if (baro_ref_set &&
	    baro_sensor_value_to_altitude(&baro->pressure, &altitude) == 0) {
		local.altitude_m = altitude;
		local.baro_ok = true;
	}
}
#endif /* CONFIG_BARO */

/**
 * @brief Sampler thread: reads each sensor as it comes due and publishes.
 */
static void sampler_task(void *, void *, void *)
{
	sensors_init();

	int64_t now = k_uptime_get();
#if defined(CONFIG_IMU)
	int64_t imu_due = now;
	struct imu_data imu_msg;
#endif /* CONFIG_IMU */
#if defined(CONFIG_BARO)
	int64_t baro_due = now;
	struct baro_data baro_msg;
#endif /* CONFIG_BARO */

	while (1) {
		now = k_uptime_get();
		int64_t next_due = now + SENSOR_IDLE_SLEEP_MS;

#if defined(CONFIG_IMU)
		if (imu_ok) {
			if (now >= imu_due) {
				/* -EAGAIN only means a triggered sensor has
				 * nothing new; a real sample is handled here.
				 */
				if (imu_poll(IMU_DEV, &imu_msg) == 0) {
					handle_imu(&imu_msg);
				}

				now = k_uptime_get();
				imu_due += IMU_PERIOD_MS;
				if (imu_due <= now) {
					imu_due = now + IMU_PERIOD_MS;
				}
			}

			next_due = MIN(next_due, imu_due);
		}
#endif /* CONFIG_IMU */

#if defined(CONFIG_BARO)
		if (baro_ok) {
			if (now >= baro_due) {
				if (baro_measure(BARO_DEV, &baro_msg) == 0) {
					handle_baro(&baro_msg);
				}

				now = k_uptime_get();
				baro_due += BARO_PERIOD_MS;
				if (baro_due <= now) {
					baro_due = now + BARO_PERIOD_MS;
				}
			}

			next_due = MIN(next_due, baro_due);
		}
#endif /* CONFIG_BARO */

		gs_set_local(&local);

		if (next_due <= now) {
			next_due = now + 1;
		}

		/* Absolute deadline, so a slow read eats into this sleep
		 * instead of adding to the period.
		 */
		k_sleep(K_TIMEOUT_ABS_MS(next_due));
	}
}

/* Preemptible; the display and receivers sit below it so a stalled bus read
 * can never cost the UI its responsiveness.
 */
K_THREAD_DEFINE(sampler, 4096, sampler_task, NULL, NULL, NULL, 6, 0, 0);

/* ============================================================
 *                     MAIN INITIALIZATION
 * ============================================================ */

/**
 * @brief Application entry point.
 *
 * Brings up the shared snapshot and the receiver/UI modules; all sampling and
 * rendering is done by threads started via K_THREAD_DEFINE.
 */
int main(void)
{
	LOG_INF("Auxspace AURORA %s - Ground Support", APP_VERSION_STRING);

	gs_init();

#if defined(CONFIG_GNSS)
	(void)gnss_rx_init();
#endif /* CONFIG_GNSS */

	(void)hc12_rx_init();

#if defined(CONFIG_LVGL)
	(void)display_init();
#endif /* CONFIG_LVGL */

	return 0;
}
