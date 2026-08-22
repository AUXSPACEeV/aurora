/**
 * @file hc12_rx.c
 * @brief HC-12 telemetry receiver: reassembles downlink frames off the UART.
 *
 * The HC-12 is a transparent 433 MHz UART bridge, so the ground side simply
 * reads the bytes the rocket's telemetry backend transmitted. The wire frame
 * mirrors lib/telemetry/hc12/hc12_internal.h:
 *
 *   [0]    0xA5   magic0
 *   [1]    0x5A   magic1
 *   [2]    type
 *   [3]    payload_len
 *   [4..]  payload (payload_len bytes)
 *   [..]   CRC-16/CCITT (seed 0xFFFF) over bytes [2 .. 3+payload_len], LE
 *
 * Bytes arrive in an ISR that only buffers them; a worker thread runs the
 * frame state machine and publishes decoded telemetry, so nothing touches a
 * mutex from interrupt context.
 *
 * Both ends are little-endian ARM, so the payload's multi-byte fields are
 * copied through without byte-swapping. A big-endian receiver would need to
 * swap them.
 *
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ground_state.h"
#include "hc12_rx.h"

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/ring_buffer.h>

LOG_MODULE_REGISTER(hc12_rx, CONFIG_GROUND_SUPPORT_LOG_LEVEL);

/* UART the HC-12 sits on, taken from the hc12 node's `uart` phandle. */
#define HC12_NODE DT_NODELABEL(hc12)
BUILD_ASSERT(DT_NODE_HAS_STATUS(HC12_NODE, okay),
	     "an enabled 'hc12' node with a uart phandle is required");
static const struct device *const hc12_uart = DEVICE_DT_GET(DT_PHANDLE(HC12_NODE, uart));

/* --- Wire format (keep in sync with hc12_internal.h) ----------------- */

#define HC12_MAGIC0         0xA5
#define HC12_MAGIC1         0x5A
#define HC12_TYPE_SM_UPDATE 0x01

/** @brief SM_UPDATE payload, packed little-endian (64 bytes). */
struct __packed hc12_sm_update_payload {
	uint32_t timestamp_ms;
	uint8_t  state;
	uint8_t  armed;
	uint8_t  sm_type;
	uint8_t  reserved;
	double   altitude;
	double   acceleration;
	double   accel_vert;
	double   velocity;
	double   orientation[3];
};
BUILD_ASSERT(sizeof(struct hc12_sm_update_payload) == 64,
	     "HC-12 SM_UPDATE payload must stay 64 bytes to match the transmitter");

#define HC12_MAX_PAYLOAD sizeof(struct hc12_sm_update_payload)

/* --- ISR -> worker plumbing ------------------------------------------ */

RING_BUF_DECLARE(hc12_rb, 512);
static K_SEM_DEFINE(hc12_rx_sem, 0, 1);

static void hc12_uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	uint8_t buf[32];

	uart_irq_update(dev);

	while (uart_irq_rx_ready(dev)) {
		int n = uart_fifo_read(dev, buf, sizeof(buf));

		if (n <= 0) {
			break;
		}

		/* Drop on overflow rather than stall the ISR: a lost frame is
		 * re-sent on the next downlink tick anyway.
		 */
		(void)ring_buf_put(&hc12_rb, buf, (uint32_t)n);
	}

	k_sem_give(&hc12_rx_sem);
}

/* --- Frame decode ---------------------------------------------------- */

static void hc12_handle_sm_update(const uint8_t *payload)
{
	struct hc12_sm_update_payload p;

	memcpy(&p, payload, sizeof(p));

	struct gs_telemetry t = {
		.valid = true,
		.rx_uptime_ms = k_uptime_get(),
		.timestamp_ms = p.timestamp_ms,
		.state = p.state,
		.type = p.sm_type,
		.armed = p.armed,
		.altitude_m = p.altitude,
		.accel_ms2 = p.acceleration,
		.accel_vert_ms2 = p.accel_vert,
		.velocity_ms = p.velocity,
		.orientation = {p.orientation[0], p.orientation[1], p.orientation[2]},
	};

	gs_set_telemetry(&t);
}

/**
 * @brief Feed one received byte through the frame state machine.
 *
 * @c crcbuf accumulates [type][len][payload], exactly the span the transmitter
 * ran the CRC over, so verification is a single crc16_ccitt() call.
 */
static void hc12_feed(uint8_t b)
{
	static enum {
		S_MAGIC0, S_MAGIC1, S_TYPE, S_LEN, S_PAYLOAD, S_CRC0, S_CRC1
	} state = S_MAGIC0;
	static uint8_t crcbuf[2 + HC12_MAX_PAYLOAD];
	static uint8_t len;
	static uint8_t idx;
	static uint8_t crc_lo;

	switch (state) {
	case S_MAGIC0:
		if (b == HC12_MAGIC0) {
			state = S_MAGIC1;
		}
		break;
	case S_MAGIC1:
		/* A stray 0xA5 could be the real start; only a non-magic byte
		 * resyncs to the hunt.
		 */
		if (b == HC12_MAGIC1) {
			state = S_TYPE;
		} else if (b != HC12_MAGIC0) {
			state = S_MAGIC0;
		}
		break;
	case S_TYPE:
		crcbuf[0] = b;
		state = S_LEN;
		break;
	case S_LEN:
		len = b;
		crcbuf[1] = b;
		if (len > HC12_MAX_PAYLOAD) {
			state = S_MAGIC0;
			break;
		}
		idx = 0;
		state = (len == 0) ? S_CRC0 : S_PAYLOAD;
		break;
	case S_PAYLOAD:
		crcbuf[2 + idx] = b;
		if (++idx >= len) {
			state = S_CRC0;
		}
		break;
	case S_CRC0:
		crc_lo = b;
		state = S_CRC1;
		break;
	case S_CRC1: {
		uint16_t rx_crc = (uint16_t)crc_lo | ((uint16_t)b << 8);
		uint16_t calc = crc16_ccitt(0xFFFF, crcbuf, (size_t)len + 2);

		if (rx_crc == calc) {
			if (crcbuf[0] == HC12_TYPE_SM_UPDATE &&
			    len == sizeof(struct hc12_sm_update_payload)) {
				hc12_handle_sm_update(&crcbuf[2]);
			}
		} else {
			LOG_WRN_RATELIMIT("HC-12 CRC mismatch (type 0x%02x len %u)",
					  crcbuf[0], len);
		}
		state = S_MAGIC0;
		break;
	}
	}
}

static void hc12_rx_task(void *, void *, void *)
{
	while (1) {
		k_sem_take(&hc12_rx_sem, K_FOREVER);

		uint8_t b;

		while (ring_buf_get(&hc12_rb, &b, 1) == 1) {
			hc12_feed(b);
		}
	}
}

/* Below the sampler (6): telemetry decode must never delay a sensor read. */
K_THREAD_DEFINE(hc12_rx, 2048, hc12_rx_task, NULL, NULL, NULL, 7, 0, 0);

int hc12_rx_init(void)
{
	if (!device_is_ready(hc12_uart)) {
		LOG_ERR("HC-12 UART %s not ready", hc12_uart->name);
		return -ENODEV;
	}

	uart_irq_rx_disable(hc12_uart);
	uart_irq_tx_disable(hc12_uart);
	uart_irq_callback_user_data_set(hc12_uart, hc12_uart_isr, NULL);
	//uart_irq_rx_enable(hc12_uart);

	LOG_INF("HC-12 RX ready on %s", hc12_uart->name);
	return 0;
}
