/**
 * @file main.c
 * @brief Unit tests for the telemetry dispatcher and HC-12 backend.
 *
 * Three suites:
 *   - format:    locks the HC-12 wire frame byte-for-byte.
 *   - rate:      exercises the per-backend rate limiter.
 *   - dispatch:  verifies fan-out to multiple registered backends and
 *                the dispatcher's error aggregation.
 *
 * UART traffic from the HC-12 worker thread is dumped into qemu's
 * uart0 (which also carries ztest output). Tests never read what
 * leaves the wire. They verify counters and frame bytes built by
 * the helper directly.
 *
 * Copyright (c) 2025-2026 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

#include <aurora/lib/telemetry.h>

#include "hc12_internal.h"

/* ==========================================================
 *                     STUB BACKENDS
 * ==========================================================
 * Two stub backends are registered at link time alongside the HC-12
 * backend. Their behaviour is controlled by these globals so each
 * test can configure return codes independently.
 */
static int  stub_a_init_calls;
static int  stub_a_send_calls;
static int  stub_a_init_rc;
static int  stub_a_send_rc;

static int  stub_b_init_calls;
static int  stub_b_send_calls;
static int  stub_b_init_rc;
static int  stub_b_send_rc;

static int stub_a_init(void) { stub_a_init_calls++; return stub_a_init_rc; }
static int stub_a_send(enum sm_state s, const struct sm_inputs *in)
{
	ARG_UNUSED(s); ARG_UNUSED(in);
	stub_a_send_calls++;
	return stub_a_send_rc;
}

static int stub_b_init(void) { stub_b_init_calls++; return stub_b_init_rc; }
static int stub_b_send(enum sm_state s, const struct sm_inputs *in)
{
	ARG_UNUSED(s); ARG_UNUSED(in);
	stub_b_send_calls++;
	return stub_b_send_rc;
}

static const struct telemetry_backend_api stub_a_api = {
	.init = stub_a_init, .send_sm_update = stub_a_send,
};
static const struct telemetry_backend_api stub_b_api = {
	.init = stub_b_init, .send_sm_update = stub_b_send,
};

TELEMETRY_BACKEND_DEFINE(stub_a, &stub_a_api);
TELEMETRY_BACKEND_DEFINE(stub_b, &stub_b_api);

static void stub_reset(void)
{
	stub_a_init_calls = 0; stub_a_send_calls = 0;
	stub_a_init_rc = 0;    stub_a_send_rc = 0;
	stub_b_init_calls = 0; stub_b_send_calls = 0;
	stub_b_init_rc = 0;    stub_b_send_rc = 0;
}

/* Minimal valid SM inputs for send_sm_update calls. */
static const struct sm_inputs DUMMY_INPUTS = {
	.armed = 1,
	.altitude = 100.0,
	.acceleration = 9.81,
	.accel_vert = 1.0,
	.velocity = 12.5,
	.orientation = { 0.1, 0.2, 0.3 },
};

/* Sleep long enough to clear any rate-limit window left over from a
 * previous test. CONFIG_AURORA_TELEMETRY_HC12_MIN_INTERVAL_MS=50.
 */
static inline void clear_rate_window(void)
{
	k_msleep(60);
}

/* ==========================================================
 *                     FORMAT SUITE
 * ==========================================================
 * Locks the HC-12 wire frame layout. Magic bytes, header field
 * positions, and the CRC algorithm are hardcoded *in the test*
 * rather than imported from the backend's headers. A silent change
 * to any of them in production must fail this suite.
 */

ZTEST(telemetry_hc12_format, test_sm_update_frame_bytes)
{
	struct hc12_sm_update_payload p = {
		.timestamp_ms = 0xDEADBEEF,
		.state        = 3,
		.armed        = 1,
		.reserved     = 0,
		.altitude     = 123.5,
		.acceleration = 9.81,
		.accel_vert   = -1.25,
		.velocity     = 42.0,
		.orientation  = { 0.5, -0.25, 1.0 },
	};

	uint8_t buf[128] = { 0 };
	size_t  n = hc12_frame_finalise(buf, sizeof(buf), 0x01, &p,
					(uint8_t)sizeof(p));

	/* Total = 2 magic + 1 type + 1 len + 64 payload + 2 CRC = 70.
	 * Payload = u32 + u8 + u8 + i16 + 4*f64 + 3*f64 = 64 bytes
	 * (packed, little-endian).
	 */
	zassert_equal(n, 70, "unexpected frame length: %zu", n);

	zassert_equal(buf[0], 0xA5, "magic0");
	zassert_equal(buf[1], 0x5A, "magic1");
	zassert_equal(buf[2], 0x01, "type=SM_UPDATE");
	zassert_equal(buf[3], 64,   "payload length byte");

	/* Payload is little-endian, packed. Check the first u32 by hand
	 * to lock byte order; trust memcpy semantics for the rest.
	 */
	zassert_equal(buf[4], 0xEF, "ts byte 0 (LE)");
	zassert_equal(buf[5], 0xBE, "ts byte 1");
	zassert_equal(buf[6], 0xAD, "ts byte 2");
	zassert_equal(buf[7], 0xDE, "ts byte 3");
	zassert_equal(buf[8], 3,    "state byte");
	zassert_equal(buf[9], 1,    "armed byte");

	/* CRC-16/CCITT (init 0xFFFF) over [type .. end of payload]. */
	uint16_t expected_crc = crc16_ccitt(0xFFFF, &buf[2], 1 + 1 + 64);
	uint16_t actual_crc   = sys_get_le16(&buf[68]);
	zassert_equal(actual_crc, expected_crc,
		      "CRC mismatch: got %04x want %04x",
		      actual_crc, expected_crc);
}

ZTEST(telemetry_hc12_format, test_buffer_too_small)
{
	struct hc12_sm_update_payload p = { 0 };
	uint8_t small[10];

	zassert_equal(hc12_frame_finalise(small, sizeof(small), 0x01, &p,
					  (uint8_t)sizeof(p)),
		      0, "should reject undersized buffer");
}

ZTEST_SUITE(telemetry_hc12_format, NULL, NULL, NULL, NULL, NULL);

/* ==========================================================
 *                     RATE LIMITER SUITE
 * ==========================================================
 * Exercises the per-backend rate limiter inside hc12_send_sm_update.
 * Compiled with CONFIG_AURORA_TELEMETRY_HC12_MIN_INTERVAL_MS=50.
 *
 * The dispatcher returns the first non-zero result from any backend.
 * Stubs A and B are silenced (rc=0) so the only path that can return
 * -EAGAIN is the HC-12 rate limiter.
 */

static void rate_before(void *fixture)
{
	ARG_UNUSED(fixture);
	stub_reset();
	(void)telemetry_init();
	clear_rate_window();
}

ZTEST(telemetry_rate, test_first_call_accepted)
{
	int rc = telemetry_send_sm_update(SM_IDLE, &DUMMY_INPUTS);

	zassert_equal(rc, 0, "first send should succeed, got %d", rc);
}

ZTEST(telemetry_rate, test_second_call_within_window_rejected)
{
	zassert_equal(telemetry_send_sm_update(SM_IDLE, &DUMMY_INPUTS), 0,
		      "first send");

	/* Burst the second one immediately. HC-12 should reject with
	 * -EAGAIN; the dispatcher returns the first non-zero rc.
	 */
	int rc = telemetry_send_sm_update(SM_IDLE, &DUMMY_INPUTS);

	zassert_equal(rc, -EAGAIN,
		      "second send should be rate-limited, got %d", rc);

	/* Stubs are silent backends, they should *still* have received
	 * both calls (rate limiting is HC-12-local, not dispatcher-wide).
	 */
	zassert_equal(stub_a_send_calls, 2,
		      "stub_a should see both sends, got %d",
		      stub_a_send_calls);
}

ZTEST(telemetry_rate, test_recovery_after_window)
{
	zassert_equal(telemetry_send_sm_update(SM_IDLE, &DUMMY_INPUTS), 0,
		      "first send");
	zassert_equal(telemetry_send_sm_update(SM_IDLE, &DUMMY_INPUTS),
		      -EAGAIN, "second send");

	clear_rate_window();

	int rc = telemetry_send_sm_update(SM_IDLE, &DUMMY_INPUTS);

	zassert_equal(rc, 0,
		      "send after window should succeed, got %d", rc);
}

ZTEST_SUITE(telemetry_rate, NULL, NULL, rate_before, NULL, NULL);

/* ==========================================================
 *                     DISPATCH SUITE
 * ==========================================================
 * Verifies the dispatcher walks every registered backend and
 * aggregates errors correctly.
 */

static void dispatch_before(void *fixture)
{
	ARG_UNUSED(fixture);
	stub_reset();
	clear_rate_window();
}

ZTEST(telemetry_dispatch, test_init_fans_out_to_all_backends)
{
	int rc = telemetry_init();

	zassert_equal(rc, 0, "init with no backend errors");
	zassert_equal(stub_a_init_calls, 1,
		      "stub_a init called exactly once (got %d)",
		      stub_a_init_calls);
	zassert_equal(stub_b_init_calls, 1,
		      "stub_b init called exactly once (got %d)",
		      stub_b_init_calls);
}

ZTEST(telemetry_dispatch, test_init_reports_first_error_but_keeps_going)
{
	stub_a_init_rc = -EIO;
	stub_b_init_rc = 0;

	int rc = telemetry_init();

	zassert_equal(rc, -EIO,
		      "dispatcher should surface the first error, got %d",
		      rc);
	zassert_equal(stub_b_init_calls, 1,
		      "stub_b must still be called after stub_a failure");
}

ZTEST(telemetry_dispatch, test_send_fans_out_to_all_backends)
{
	(void)telemetry_init();
	stub_reset();
	clear_rate_window();

	int rc = telemetry_send_sm_update(SM_IDLE, &DUMMY_INPUTS);

	zassert_equal(rc, 0, "all backends accepted");
	zassert_equal(stub_a_send_calls, 1,
		      "stub_a saw the send (got %d)", stub_a_send_calls);
	zassert_equal(stub_b_send_calls, 1,
		      "stub_b saw the send (got %d)", stub_b_send_calls);
}

ZTEST(telemetry_dispatch, test_send_surfaces_first_error)
{
	(void)telemetry_init();
	stub_reset();
	clear_rate_window();

	stub_a_send_rc = -EIO;
	stub_b_send_rc = -ENOSPC;

	int rc = telemetry_send_sm_update(SM_IDLE, &DUMMY_INPUTS);

	zassert_true(rc == -EIO || rc == -ENOSPC,
		     "dispatcher returned an error from one of the stubs "
		     "(got %d)", rc);
	zassert_equal(stub_b_send_calls, 1,
		      "stub_b must run even after stub_a errors");
}

ZTEST_SUITE(telemetry_dispatch, NULL, NULL, dispatch_before, NULL, NULL);
