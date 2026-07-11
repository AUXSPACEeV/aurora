/**
 * @file main.c
 * @brief Unit tests for the pad_link wire format and snapshot pipeline.
 *
 * Two suites:
 *   - format:  locks the byte layout of pl_raw_payload and
 *              pl_computed_payload so the Python central's hard-coded
 *              struct.unpack strings keep decoding correctly.
 *   - snap:    publishes synthetic IMU/baro samples on zbus and calls
 *              pad_link_publish_sm(), then peeks the internal snapshot
 *              through pad_link_test_get_snapshot() to verify packing
 *              and field ordering.
 *
 * bt_enable() is intentionally never called: pad_link_publish_sm
 * early-exits when current_conn is NULL, so we exercise the
 * snapshot-update path without bringing up the BT host. Board
 * capability flags are only computed inside pad_link_init(), so the
 * boardcap test calls pad_link_test_trigger_boardcap() to run that
 * same logic directly.
 *
 * Copyright (c) 2026 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>
#include <zephyr/zbus/zbus.h>

#include <aurora/lib/baro.h>
#include <aurora/lib/imu.h>
#include <aurora/lib/pad_link.h>
#include <aurora/lib/state/state.h>

#include "pad_link_wire.h"

/* ==========================================================
 *                     FORMAT SUITE
 * ==========================================================
 * Locks the on-the-wire byte layout. The Python central decodes raw
 * and computed payloads with hard-coded struct strings; a silent
 * field reorder here must fail this suite before it reaches the
 * central.
 */

ZTEST(pad_link_format, test_boardcap_flags)
{
	/* IMU group — byte 0 */
	zassert_equal(PL_CAP_IMU_TYPE_NONE, 0x0u, "IMU type NONE");
	zassert_equal(PL_CAP_IMU_TYPE_6DOF, 0x1u, "IMU type 6-DoF");
	zassert_equal(PL_CAP_IMU_TYPE_9DOF, 0x2u, "IMU type 9-DoF");
	zassert_equal(PL_CAP_IMU_TYPE_MASK, 0x7u, "IMU type mask");
	zassert_equal(PL_CAP_ACCEL, (1u << 3), "accel flag");
	zassert_equal(PL_CAP_GYRO,  (1u << 4), "gyro flag");

	/* Environmental group — byte 1 */
	zassert_equal(PL_CAP_BARO,       (1u << 8),  "baro flag");
	zassert_equal(PL_CAP_TEMP_INNER, (1u << 9),  "inner temp flag");
	zassert_equal(PL_CAP_TEMP_MOTOR, (1u << 10), "motor temp flag");
	zassert_equal(PL_CAP_TEMP_HULL,  (1u << 11), "hull temp flag");

	/* Positioning group — byte 2 */
	zassert_equal(PL_CAP_GPS, (1u << 16), "GPS flag");
}

ZTEST(pad_link_format, test_baro_payload_layout)
{
	zassert_equal(sizeof(struct pl_baro_payload), 20,
		      "baro payload size drifted: %zu",
		      sizeof(struct pl_baro_payload));

	zassert_equal(offsetof(struct pl_baro_payload, uptime_ms),  0,  "uptime_ms");
	zassert_equal(offsetof(struct pl_baro_payload, temp_us),    4,  "temp_us");
	zassert_equal(offsetof(struct pl_baro_payload, press_us),   12, "press_us");
}

ZTEST(pad_link_format, test_accel_payload_layout)
{
	zassert_equal(sizeof(struct pl_accel_payload), 28,
		      "accel payload size drifted: %zu",
		      sizeof(struct pl_accel_payload));

	zassert_equal(offsetof(struct pl_accel_payload, uptime_ms),  0, "uptime_ms");
	zassert_equal(offsetof(struct pl_accel_payload, accel_us),   4, "accel_us");
}

ZTEST(pad_link_format, test_gyro_payload_layout)
{
	zassert_equal(sizeof(struct pl_gyro_payload), 28,
		      "gyro payload size drifted: %zu",
		      sizeof(struct pl_gyro_payload));

	zassert_equal(offsetof(struct pl_gyro_payload, uptime_ms),  0, "uptime_ms");
	zassert_equal(offsetof(struct pl_gyro_payload, gyro_us),    4, "gyro_us");
}

ZTEST(pad_link_format, test_imu6_payload_layout)
{
	zassert_equal(sizeof(struct pl_imu6_payload), 52,
		      "imu6 payload size drifted: %zu",
		      sizeof(struct pl_imu6_payload));

	zassert_equal(offsetof(struct pl_imu6_payload, uptime_ms),  0,  "uptime_ms");
	zassert_equal(offsetof(struct pl_imu6_payload, accel_us),   4,  "accel_us");
	zassert_equal(offsetof(struct pl_imu6_payload, gyro_us),    28, "gyro_us");
}

ZTEST(pad_link_format, test_inner_temp_payload_layout)
{
	zassert_equal(sizeof(struct pl_inner_temp_payload), 12,
		      "inner_temp payload size drifted: %zu",
		      sizeof(struct pl_inner_temp_payload));

	zassert_equal(offsetof(struct pl_inner_temp_payload, uptime_ms),  0, "uptime_ms");
	zassert_equal(offsetof(struct pl_inner_temp_payload, temp_us),    4, "temp_us");
}

ZTEST(pad_link_format, test_raw_payload_layout)
{
	zassert_equal(sizeof(struct pl_raw_payload), 68,
		      "raw payload size drifted: %zu",
		      sizeof(struct pl_raw_payload));

	zassert_equal(offsetof(struct pl_raw_payload, uptime_ms),   0,  "uptime_ms");
	zassert_equal(offsetof(struct pl_raw_payload, accel_val1),  4,  "accel_val1");
	zassert_equal(offsetof(struct pl_raw_payload, accel_val2),  16, "accel_val2");
	zassert_equal(offsetof(struct pl_raw_payload, gyro_val1),   28, "gyro_val1");
	zassert_equal(offsetof(struct pl_raw_payload, gyro_val2),   40, "gyro_val2");
	zassert_equal(offsetof(struct pl_raw_payload, temp_val1),   52, "temp_val1");
	zassert_equal(offsetof(struct pl_raw_payload, temp_val2),   56, "temp_val2");
	zassert_equal(offsetof(struct pl_raw_payload, press_val1),  60, "press_val1");
	zassert_equal(offsetof(struct pl_raw_payload, press_val2),  64, "press_val2");
}

ZTEST(pad_link_format, test_computed_payload_layout)
{
	zassert_equal(sizeof(struct pl_computed_payload), 28,
		      "computed payload size drifted: %zu",
		      sizeof(struct pl_computed_payload));

	zassert_equal(offsetof(struct pl_computed_payload, uptime_ms),  0,  "uptime_ms");
	zassert_equal(offsetof(struct pl_computed_payload, altitude),   4,  "altitude");
	zassert_equal(offsetof(struct pl_computed_payload, velocity),   8,  "velocity");
	zassert_equal(offsetof(struct pl_computed_payload, yaw),        12, "yaw");
	zassert_equal(offsetof(struct pl_computed_payload, pitch),      16, "pitch");
	zassert_equal(offsetof(struct pl_computed_payload, roll),       20, "roll");
	zassert_equal(offsetof(struct pl_computed_payload, accel_vert), 24, "accel_vert");
}

ZTEST(pad_link_format, test_computed_payload_bytes)
{
	/* Build a known payload and check the first u32 byte-for-byte to
	 * lock little-endian wire order. The Python central reads this
	 * with struct.unpack("<Iffffff", ...).
	 */
	struct pl_computed_payload p = {
		.uptime_ms  = 0xDEADBEEF,
		.altitude   = 1.0f,
		.velocity   = 2.0f,
		.yaw        = 3.0f,
		.pitch      = 4.0f,
		.roll       = 5.0f,
		.accel_vert = 6.0f,
	};

	uint8_t buf[sizeof(p)];
	memcpy(buf, &p, sizeof(p));

	zassert_equal(buf[0], 0xEF, "uptime_ms byte 0 (LE)");
	zassert_equal(buf[1], 0xBE, "uptime_ms byte 1");
	zassert_equal(buf[2], 0xAD, "uptime_ms byte 2");
	zassert_equal(buf[3], 0xDE, "uptime_ms byte 3");

	/* Each float occupies four bytes immediately following uptime_ms. */
	float f;
	memcpy(&f, &buf[4],  sizeof(f)); zassert_equal(f, 1.0f, "altitude");
	memcpy(&f, &buf[8],  sizeof(f)); zassert_equal(f, 2.0f, "velocity");
	memcpy(&f, &buf[12], sizeof(f)); zassert_equal(f, 3.0f, "yaw");
	memcpy(&f, &buf[16], sizeof(f)); zassert_equal(f, 4.0f, "pitch");
	memcpy(&f, &buf[20], sizeof(f)); zassert_equal(f, 5.0f, "roll");
	memcpy(&f, &buf[24], sizeof(f)); zassert_equal(f, 6.0f, "accel_vert");
}

ZTEST_SUITE(pad_link_format, NULL, NULL, NULL, NULL, NULL);

/* ==========================================================
 *                     SNAP SUITE
 * ==========================================================
 * Drives the zbus listeners and the publish entry point, then peeks
 * the internal snapshot to confirm packing.
 */

ZTEST(pad_link_snap, test_imu_publish_updates_snap_raw)
{
	struct imu_data msg = {
		.accel = {
			{ .val1 = 1,  .val2 = 100 },
			{ .val1 = 2,  .val2 = 200 },
			{ .val1 = 3,  .val2 = 300 },
		},
		.gyro = {
			{ .val1 = 10, .val2 = 1000 },
			{ .val1 = 20, .val2 = 2000 },
			{ .val1 = 30, .val2 = 3000 },
		},
	};

	/* k_uptime_get_32() returns 0 during the very first millisecond
	 * of boot, so let the kernel tick at least once before publishing.
	 * Otherwise the "uptime_ms was stamped" check fires on a real zero.
	 */
	k_msleep(2);
	uint32_t before = k_uptime_get_32();

	zassert_ok(zbus_chan_pub(&imu_data_chan, &msg, K_SECONDS(1)),
		   "imu publish");

	struct pl_raw_payload raw;
	pad_link_test_get_snapshot(NULL, NULL, &raw, NULL,
				   NULL, NULL, NULL, NULL, NULL, NULL);

	for (int i = 0; i < 3; i++) {
		zassert_equal(raw.accel_val1[i], msg.accel[i].val1,
			      "accel[%d].val1", i);
		zassert_equal(raw.accel_val2[i], msg.accel[i].val2,
			      "accel[%d].val2", i);
		zassert_equal(raw.gyro_val1[i], msg.gyro[i].val1,
			      "gyro[%d].val1", i);
		zassert_equal(raw.gyro_val2[i], msg.gyro[i].val2,
			      "gyro[%d].val2", i);
	}
	zassert_true(raw.uptime_ms >= before,
		     "uptime_ms not stamped: got %u, expected >= %u",
		     raw.uptime_ms, before);
}

ZTEST(pad_link_snap, test_baro_publish_updates_snap_raw)
{
	struct baro_data msg = {
		.temperature = { .val1 = 23, .val2 = 456000 },
		.pressure    = { .val1 = 101, .val2 = 325000 },
	};

	zassert_ok(zbus_chan_pub(&baro_data_chan, &msg, K_SECONDS(1)),
		   "baro publish");

	struct pl_raw_payload raw;
	pad_link_test_get_snapshot(NULL, NULL, &raw, NULL,
				   NULL, NULL, NULL, NULL, NULL, NULL);

	zassert_equal(raw.temp_val1,  msg.temperature.val1, "temp_val1");
	zassert_equal(raw.temp_val2,  msg.temperature.val2, "temp_val2");
	zassert_equal(raw.press_val1, msg.pressure.val1,    "press_val1");
	zassert_equal(raw.press_val2, msg.pressure.val2,    "press_val2");
}

ZTEST(pad_link_snap, test_publish_sm_updates_snap_comp)
{
	const struct sm_inputs in = {
		.armed        = 1,
		.altitude     = 123.5,
		.acceleration = 9.81,
		.accel_vert   = -1.25,
		.velocity     = 42.0,
		.orientation  = { 0.5, -0.25, 1.0 },
	};

	pad_link_publish_sm(SM_BOOST, SM_TYPE_SIMPLE, &in);

	uint8_t sm_type, sm_state;
	struct pl_computed_payload comp;
	pad_link_test_get_snapshot(&sm_type, &sm_state, NULL, &comp,
				   NULL, NULL, NULL, NULL, NULL, NULL);

	zassert_equal(sm_state, (uint8_t)SM_BOOST,      "sm_state byte");
	zassert_equal(sm_type,  (uint8_t)SM_TYPE_SIMPLE, "sm_type byte");

	/* sm_inputs carries doubles; the wire payload narrows to float.
	 * Compare against the float cast so a future widening is caught.
	 */
	zassert_equal(comp.altitude,   (float)in.altitude,       "altitude");
	zassert_equal(comp.velocity,   (float)in.velocity,       "velocity");
	zassert_equal(comp.yaw,        (float)in.orientation[0], "yaw");
	zassert_equal(comp.pitch,      (float)in.orientation[1], "pitch");
	zassert_equal(comp.roll,       (float)in.orientation[2], "roll");
	zassert_equal(comp.accel_vert, (float)in.accel_vert,     "accel_vert");
	zassert_not_equal(comp.uptime_ms, 0, "uptime_ms should be stamped");
}

ZTEST(pad_link_snap, test_publish_sm_null_inputs_is_noop)
{
	/* Capture current snapshot, call with NULL, snapshot must be
	 * byte-identical. Locks the early-return contract documented in
	 * pad_link.h.
	 */
	struct pl_computed_payload before, after;
	uint8_t st_before, st_after;

	pad_link_test_get_snapshot(NULL, &st_before, NULL, &before,
				   NULL, NULL, NULL, NULL, NULL, NULL);
	pad_link_publish_sm(SM_LANDED, SM_TYPE_SIMPLE, NULL);
	pad_link_test_get_snapshot(NULL, &st_after,  NULL, &after,
				   NULL, NULL, NULL, NULL, NULL, NULL);

	zassert_equal(st_before, st_after, "sm_state must not move");
	zassert_mem_equal(&before, &after, sizeof(before),
			  "comp payload must not move");
}

ZTEST(pad_link_snap, test_boardcap_flags_set)
{
	/* prj.conf enables both CONFIG_IMU and CONFIG_BARO. */
	pad_link_test_trigger_boardcap();

	uint32_t cap;
	pad_link_test_get_snapshot(NULL, NULL, NULL, NULL,
				   &cap, NULL, NULL, NULL, NULL, NULL);

	/* IMU group: 6-DoF type, accel and gyro present */
	zassert_equal(cap & PL_CAP_IMU_TYPE_MASK,
		      PL_CAP_IMU_TYPE(PL_CAP_IMU_TYPE_6DOF),
		      "IMU type should be 6-DoF");
	zassert_true(cap & PL_CAP_ACCEL, "accel flag should be set");
	zassert_true(cap & PL_CAP_GYRO,  "gyro flag should be set");

	/* Environmental group: baro and inner temp present */
	zassert_true(cap & PL_CAP_BARO,       "baro flag should be set");
	zassert_true(cap & PL_CAP_TEMP_INNER, "inner_temp flag should be set");

	/* Positioning: no GPS source yet */
	zassert_false(cap & PL_CAP_GPS, "GPS flag should not be set");
}

ZTEST(pad_link_snap, test_baro_publish_updates_snap_baro)
{
	struct baro_data msg = {
		.temperature = { .val1 = 23,  .val2 = 456000 },
		.pressure    = { .val1 = 101, .val2 = 325000 },
	};

	/* k_uptime_get_32() returns 0 during the very first millisecond
	 * of boot; see test_imu_publish_updates_snap_raw for the same
	 * workaround.
	 */
	k_msleep(2);

	zassert_ok(zbus_chan_pub(&baro_data_chan, &msg, K_SECONDS(1)),
		   "baro publish");

	struct pl_baro_payload baro;
	pad_link_test_get_snapshot(NULL, NULL, NULL, NULL,
				   NULL, &baro, NULL, NULL, NULL, NULL);

	int64_t exp_temp  = (int64_t)23  * 1000000LL + 456000;
	int64_t exp_press = (int64_t)101 * 1000000LL + 325000;

	zassert_equal(baro.temp_us,  exp_temp,  "temp_us mismatch");
	zassert_equal(baro.press_us, exp_press, "press_us mismatch");
}

ZTEST(pad_link_snap, test_baro_publish_updates_snap_inner_temp)
{
	struct baro_data msg = {
		.temperature = { .val1 = 25, .val2 = 100000 },
		.pressure    = { .val1 = 99, .val2 = 0 },
	};

	zassert_ok(zbus_chan_pub(&baro_data_chan, &msg, K_SECONDS(1)),
		   "baro publish");

	struct pl_inner_temp_payload it;
	pad_link_test_get_snapshot(NULL, NULL, NULL, NULL,
				   NULL, NULL, NULL, NULL, NULL, &it);

	int64_t exp = (int64_t)25 * 1000000LL + 100000;

	zassert_equal(it.temp_us, exp, "inner_temp.temp_us mismatch");
}

ZTEST(pad_link_snap, test_imu_publish_updates_snap_accel)
{
	struct imu_data msg = {
		.accel = {
			{ .val1 = 1, .val2 = 100 },
			{ .val1 = 2, .val2 = 200 },
			{ .val1 = 3, .val2 = 300 },
		},
		.gyro = { {0}, {0}, {0} },
	};

	zassert_ok(zbus_chan_pub(&imu_data_chan, &msg, K_SECONDS(1)),
		   "imu publish");

	struct pl_accel_payload accel;
	pad_link_test_get_snapshot(NULL, NULL, NULL, NULL,
				   NULL, NULL, &accel, NULL, NULL, NULL);

	for (int i = 0; i < 3; i++) {
		int64_t exp = (int64_t)msg.accel[i].val1 * 1000000LL
			      + msg.accel[i].val2;
		zassert_equal(accel.accel_us[i], exp, "accel_us[%d]", i);
	}
}

ZTEST(pad_link_snap, test_imu_publish_updates_snap_gyro)
{
	struct imu_data msg = {
		.accel = { {0}, {0}, {0} },
		.gyro  = {
			{ .val1 = 10, .val2 = 1000 },
			{ .val1 = 20, .val2 = 2000 },
			{ .val1 = 30, .val2 = 3000 },
		},
	};

	zassert_ok(zbus_chan_pub(&imu_data_chan, &msg, K_SECONDS(1)),
		   "imu publish");

	struct pl_gyro_payload gyro;
	pad_link_test_get_snapshot(NULL, NULL, NULL, NULL,
				   NULL, NULL, NULL, &gyro, NULL, NULL);

	for (int i = 0; i < 3; i++) {
		int64_t exp = (int64_t)msg.gyro[i].val1 * 1000000LL
			      + msg.gyro[i].val2;
		zassert_equal(gyro.gyro_us[i], exp, "gyro_us[%d]", i);
	}
}

ZTEST(pad_link_snap, test_imu_publish_updates_snap_imu6)
{
	struct imu_data msg = {
		.accel = {
			{ .val1 = 5, .val2 = 500 },
			{ .val1 = 6, .val2 = 600 },
			{ .val1 = 7, .val2 = 700 },
		},
		.gyro = {
			{ .val1 = 50, .val2 = 5000 },
			{ .val1 = 60, .val2 = 6000 },
			{ .val1 = 70, .val2 = 7000 },
		},
	};

	zassert_ok(zbus_chan_pub(&imu_data_chan, &msg, K_SECONDS(1)),
		   "imu publish");

	struct pl_imu6_payload imu6;
	pad_link_test_get_snapshot(NULL, NULL, NULL, NULL,
				   NULL, NULL, NULL, NULL, &imu6, NULL);

	for (int i = 0; i < 3; i++) {
		int64_t exp_a = (int64_t)msg.accel[i].val1 * 1000000LL
				+ msg.accel[i].val2;
		int64_t exp_g = (int64_t)msg.gyro[i].val1 * 1000000LL
				+ msg.gyro[i].val2;
		zassert_equal(imu6.accel_us[i], exp_a, "imu6.accel_us[%d]", i);
		zassert_equal(imu6.gyro_us[i],  exp_g, "imu6.gyro_us[%d]",  i);
	}
}

ZTEST_SUITE(pad_link_snap, NULL, NULL, NULL, NULL, NULL);
