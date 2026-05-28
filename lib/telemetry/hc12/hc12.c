/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT auxspaceev_hc12

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

#include <aurora/lib/telemetry.h>

#include "hc12_internal.h"

LOG_MODULE_REGISTER(telemetry_hc12, CONFIG_AURORA_TELEMETRY_LOG_LEVEL);

BUILD_ASSERT(DT_HAS_COMPAT_STATUS_OKAY(auxspaceev_hc12),
	     "An auxspaceev,hc12 node must be enabled in devicetree");

#define HC12_NODE DT_INST(0, auxspaceev_hc12)

const struct device *const hc12_uart_dev =
	DEVICE_DT_GET(DT_INST_PHANDLE(0, uart));

/* set_gpio.port == NULL when SET is not wired in DT: runtime AT is
 * unavailable in that case (the shell command refuses, init still
 * succeeds).
 */
static const struct gpio_dt_spec set_gpio =
	GPIO_DT_SPEC_INST_GET_OR(0, set_gpios, {0});

K_MUTEX_DEFINE(hc12_uart_lock);

/* Wire frame:
 *   [0]    magic0 = 0xA5
 *   [1]    magic1 = 0x5A
 *   [2]    type
 *   [3]    len    (payload bytes excluding CRC)
 *   [4:]   payload
 *   [-2:]  crc16-ccitt over bytes [2 .. 4+len-1], little-endian
 */
#define HC12_MAGIC0 0xA5
#define HC12_MAGIC1 0x5A
#define HC12_HDR    4
#define HC12_CRC    2

#define HC12_TYPE_SM_UPDATE 0x01

struct __packed sm_update_payload {
	uint32_t timestamp_ms;
	uint8_t  state;
	uint8_t  armed;
	int16_t  reserved;
	float    altitude;
	float    acceleration;
	float    accel_vert;
	float    velocity;
	float    orientation[3];
};

#define MAX_PAYLOAD sizeof(struct sm_update_payload)
#define MAX_FRAME   (HC12_HDR + MAX_PAYLOAD + HC12_CRC)

struct hc12_frame {
	uint8_t len;
	uint8_t buf[MAX_FRAME];
};

K_MSGQ_DEFINE(tx_msgq, sizeof(struct hc12_frame),
	      CONFIG_AURORA_TELEMETRY_HC12_QUEUE_DEPTH, 4);

static atomic_t ready = ATOMIC_INIT(0);

static void frame_finalise(struct hc12_frame *f, uint8_t type,
			   const void *payload, uint8_t payload_len)
{
	f->buf[0] = HC12_MAGIC0;
	f->buf[1] = HC12_MAGIC1;
	f->buf[2] = type;
	f->buf[3] = payload_len;
	memcpy(&f->buf[HC12_HDR], payload, payload_len);

	uint16_t crc = crc16_ccitt(0xFFFF, &f->buf[2],
				   (size_t)payload_len + 2);
	sys_put_le16(crc, &f->buf[HC12_HDR + payload_len]);

	f->len = HC12_HDR + payload_len + HC12_CRC;
}

static int hc12_send_sm_update(enum sm_state state,
			       const struct sm_inputs *inputs)
{
	if (!atomic_get(&ready)) {
		return -ENODEV;
	}

#if CONFIG_AURORA_TELEMETRY_HC12_MIN_INTERVAL_MS > 0
	static int64_t last_send_ms;
	static struct k_spinlock rl_lock;

	int64_t now_ms = k_uptime_get();
	k_spinlock_key_t key = k_spin_lock(&rl_lock);
	if (now_ms - last_send_ms <
	    CONFIG_AURORA_TELEMETRY_HC12_MIN_INTERVAL_MS) {
		k_spin_unlock(&rl_lock, key);
		return -EAGAIN;
	}
	last_send_ms = now_ms;
	k_spin_unlock(&rl_lock, key);
#endif

	struct sm_update_payload p = {
		.timestamp_ms = (uint32_t)k_uptime_get(),
		.state        = (uint8_t)state,
		.armed        = inputs->armed ? 1 : 0,
		.altitude     = (float)inputs->altitude,
		.acceleration = (float)inputs->acceleration,
		.accel_vert   = (float)inputs->accel_vert,
		.velocity     = (float)inputs->velocity,
		.orientation  = {
			(float)inputs->orientation[0],
			(float)inputs->orientation[1],
			(float)inputs->orientation[2],
		},
	};

	struct hc12_frame f;
	frame_finalise(&f, HC12_TYPE_SM_UPDATE, &p, (uint8_t)sizeof(p));

	if (k_msgq_put(&tx_msgq, &f, K_NO_WAIT) != 0) {
		return -ENOMEM;
	}
	return 0;
}

static void hc12_tx_task(void *, void *, void *)
{
	struct hc12_frame f;

	while (1) {
		(void)k_msgq_get(&tx_msgq, &f, K_FOREVER);

		/* Hold the UART lock for the whole frame: an in-flight
		 * AT exchange has reconfigured the line to 9600 baud
		 * and pulled SET low, so writing here would garble
		 * both the frame and the AT command.
		 */
		k_mutex_lock(&hc12_uart_lock, K_FOREVER);
		for (uint8_t i = 0; i < f.len; i++) {
			uart_poll_out(hc12_uart_dev, f.buf[i]);
		}
		k_mutex_unlock(&hc12_uart_lock);
	}
}

K_THREAD_DEFINE(hc12_tx, CONFIG_AURORA_TELEMETRY_HC12_STACK_SIZE,
		hc12_tx_task, NULL, NULL, NULL,
		CONFIG_AURORA_TELEMETRY_HC12_THREAD_PRIORITY, 0, 0);

static int hc12_init(void)
{
	if (!device_is_ready(hc12_uart_dev)) {
		LOG_ERR("UART %s not ready", hc12_uart_dev->name);
		return -ENODEV;
	}

	if (set_gpio.port) {
		if (!gpio_is_ready_dt(&set_gpio)) {
			LOG_ERR("SET GPIO not ready");
			return -ENODEV;
		}
		/* Inactive (high w.r.t. line; the binding flags ACTIVE_LOW
		 * so "inactive" means transparent mode). */
		int rc = gpio_pin_configure_dt(&set_gpio, GPIO_OUTPUT_INACTIVE);
		if (rc) {
			LOG_ERR("SET GPIO configure failed (%d)", rc);
			return rc;
		}
	} else {
		LOG_INF("HC-12 SET pin not wired: runtime AT disabled");
	}

	atomic_set(&ready, 1);
	LOG_INF("HC-12 backend up on %s", hc12_uart_dev->name);
	return 0;
}

static const struct telemetry_backend_api hc12_api = {
	.init           = hc12_init,
	.send_sm_update = hc12_send_sm_update,
};

TELEMETRY_BACKEND_DEFINE(hc12, &hc12_api);
