/*
 * Copyright (c) 2025-2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAKE_SENSORS_REPLAY_H_
#define FAKE_SENSORS_REPLAY_H_

#include <stddef.h>
#include <stdint.h>

struct replay_imu_sample {
	uint64_t t_ns;
	float x, y, z;
};

struct replay_baro_sample {
	uint64_t t_ns;
	float pres_kpa, temp_c;
};

extern const struct replay_imu_sample replay_accel[];
extern const size_t replay_accel_len;
extern const struct replay_imu_sample replay_gyro[];
extern const size_t replay_gyro_len;
extern const struct replay_baro_sample replay_baro[];
extern const size_t replay_baro_len;

#endif /* FAKE_SENSORS_REPLAY_H_ */
