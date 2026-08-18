/**
 * @file fake_sensors.h
 * @brief Hardware interface supplied by the simulated sensor backends.
 *
 * The synthetic profile generator (fake_sensors.c) and the recorded-flight
 * playback engine (fake_sensors_replay.c) are alternative implementations of
 * this interface, selected by CONFIG_AURORA_FAKE_SENSORS_SYNTH /
 * CONFIG_AURORA_FAKE_SENSORS_REPLAY.  Exactly one of them is compiled in.
 *
 * They supply samples only.  The schedule they are read on belongs to
 * main.c's sensor thread, which drives them through the same slot table it
 * uses for real hardware, so the thread layout, priorities and pacing of a
 * simulated build match the real one.  The @c dev argument exists solely so
 * these match the fetch signature of imu_poll() / baro_measure(); it is
 * unused and callers pass NULL.
 *
 * Copyright (c) 2025-2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAKE_SENSORS_H_
#define FAKE_SENSORS_H_

#include <zephyr/device.h>

/**
 * @brief Prepare the simulated IMU source.
 *
 * @retval 0 on success.
 * @retval -errno if the backend has no usable sample data.
 */
int fake_imu_init(void);

/**
 * @brief Produce one simulated IMU sample and publish it to the z-bus.
 *
 * @param dev Unused; present to match the real fetch signature.
 *
 * @retval 0 on success.
 * @retval -errno if publishing failed.
 */
int fake_imu_poll(const struct device *dev);

/**
 * @brief Prepare the simulated barometer source.
 *
 * @retval 0 on success.
 * @retval -errno if the backend has no usable sample data.
 */
int fake_baro_init(void);

/**
 * @brief Produce one simulated baro sample and publish it to the z-bus.
 *
 * @param dev Unused; present to match the real fetch signature.
 *
 * @retval 0 on success.
 * @retval -errno if publishing failed.
 */
int fake_baro_measure(const struct device *dev);

/**
 * @brief Notify the automatic launch sequence that attitude calibration is
 *        complete.
 *
 * Called from the state machine thread; only the CONFIG_AURORA_SIM_AUTOTEST
 * autolaunch thread acts on it.
 */
void fake_sensors_on_calibrated(void);

#endif /* FAKE_SENSORS_H_ */
