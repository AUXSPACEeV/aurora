/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AURORA_LIB_PAD_LINK_WIRE_H_
#define AURORA_LIB_PAD_LINK_WIRE_H_

#include <stdint.h>
//#include <zephyr/drivers/sensor.h>
#include <zephyr/toolchain.h>

/* Private to the pad_link implementation and its unit tests.
 *
 * Wire layouts pinned by tests/lib/pad_link. Any field reorder, type
 * change, or insertion will break those tests *before* the Python
 * central silently decodes garbage.
 */

/* Bit flags exposed via the boardcap (a0) characteristic.
 * PL_BOARDCAP_IMU  set → a2 (accel), a3 (gyro), a4 (6-DoF) are valid.
 * PL_BOARDCAP_BARO set → a1 (baro), a7 (inner_temp) are valid.
 */
#define PL_BOARDCAP_IMU  (1U << 0)
#define PL_BOARDCAP_BARO (1U << 1)

/* Combine a Zephyr sensor_value into a single micro-unit int64.
 * physical = result / 1_000_000  (e.g. µ°C, µPa, µm/s², µrad/s)
 */
static inline int64_t sv_to_i64(const struct sensor_value *sv)
{
	return (int64_t)sv->val1 * 1000000LL + sv->val2;
}

/* Raw sensor snapshot (deprecated a3). sensor_value (val1.val2) preserved
 * so the central reconstructs full precision; uptime_ms is the time of the
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

/* Baro (a1): pressure + temperature in micro-units. 20 bytes.
 * Python: struct.unpack("<Iqq", data[:20]) → uptime_ms, temp_us, press_us
 */
struct __packed pl_baro_payload {
	uint32_t uptime_ms;  /* offset  0 */
	int64_t  temp_us;    /* offset  4 — micro-°C  */
	int64_t  press_us;   /* offset 12 — micro-Pa  */
};

/* Accelerometer (a2): 3-axis in micro-m/s². 28 bytes.
 * Python: struct.unpack("<Iqqq", data[:28]) → uptime_ms, x, y, z
 */
struct __packed pl_accel_payload {
	uint32_t uptime_ms;    /* offset  0 */
	int64_t  accel_us[3]; /* offset  4 — µm/s² x,y,z */
};

/* Gyrometer (a3): 3-axis in micro-rad/s. 28 bytes.
 * Python: struct.unpack("<Iqqq", data[:28]) → uptime_ms, x, y, z
 */
struct __packed pl_gyro_payload {
	uint32_t uptime_ms;   /* offset  0 */
	int64_t  gyro_us[3];  /* offset  4 — µrad/s x,y,z */
};

/* 6-DoF IMU (a4): accel + gyro. 52 bytes.
 * Python: struct.unpack("<Iqqqqqq", data[:52])
 *         → uptime_ms, ax, ay, az (µm/s²), gx, gy, gz (µrad/s)
 * Note: subscribe to a4 OR a2+a3, not all three — they carry the same data.
 */
struct __packed pl_imu6_payload {
	uint32_t uptime_ms;    /* offset  0 */
	int64_t  accel_us[3]; /* offset  4 — µm/s² x,y,z */
	int64_t  gyro_us[3];  /* offset 28 — µrad/s x,y,z */
};

/* Inner temperature (a7): sourced from baro sensor. 12 bytes.
 * Python: struct.unpack("<Iq", data[:12]) → uptime_ms, temp_us
 */
struct __packed pl_inner_temp_payload {
	uint32_t uptime_ms;  /* offset  0 */
	int64_t  temp_us;    /* offset  4 — micro-°C */
};

#if defined(CONFIG_ZTEST)
/* Test-only window into the internal snapshot. Each pointer may be
 * NULL to skip that field. Takes the spinlock; safe to call from any
 * context the production code is.
 */
void pad_link_test_get_snapshot(uint8_t *sm_type,
				uint8_t *sm_state,
				struct pl_raw_payload *raw,
				struct pl_computed_payload *comp,
				uint32_t *boardcap,
				struct pl_baro_payload *baro,
				struct pl_accel_payload *accel,
				struct pl_gyro_payload *gyro,
				struct pl_imu6_payload *imu6,
				struct pl_inner_temp_payload *inner_temp);

void pad_link_test_trigger_boardcap(void);
#endif

#endif /* AURORA_LIB_PAD_LINK_WIRE_H_ */
