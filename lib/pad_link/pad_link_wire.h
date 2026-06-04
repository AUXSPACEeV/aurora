/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AURORA_LIB_PAD_LINK_WIRE_H_
#define AURORA_LIB_PAD_LINK_WIRE_H_

#include <stdint.h>
#include <zephyr/toolchain.h>

/* Private to the pad_link implementation and its unit tests.
 *
 * Wire layouts pinned by tests/lib/pad_link. Any field reorder, type
 * change, or insertion will break those tests *before* the Python
 * central silently decodes garbage.
 */

/* Raw sensor snapshot. sensor_value (val1.val2) preserved so the
 * central reconstructs full precision; uptime_ms is the time of the
 * most recent contributing publish (IMU or baro), not both.
 */
struct __packed pl_raw_payload {
	uint32_t uptime_ms;
	int32_t  accel_val1[3];
	int32_t  accel_val2[3];
	int32_t  gyro_val1[3];
	int32_t  gyro_val2[3];
	int32_t  temp_val1;
	int32_t  temp_val2;
	int32_t  press_val1;
	int32_t  press_val2;
};

/* Computed kinematics. Doubles narrowed to float, enough for status. */
struct __packed pl_computed_payload {
	uint32_t uptime_ms;
	float    altitude;
	float    velocity;
	float    yaw;
	float    pitch;
	float    roll;
	float    accel_vert;
};

#if defined(CONFIG_ZTEST)
/* Test-only window into the internal snapshot. Each pointer may be
 * NULL to skip that field. Takes the spinlock; safe to call from any
 * context the production code is.
 */
void pad_link_test_get_snapshot(uint8_t *sm_type,
				uint8_t *sm_state,
				struct pl_raw_payload *raw,
				struct pl_computed_payload *comp);
#endif

#endif /* AURORA_LIB_PAD_LINK_WIRE_H_ */
