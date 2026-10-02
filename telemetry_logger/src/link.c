/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The AUX-Stack data-connector UART: the wire from the abby receiver.
 *
 * Drains the UART and reassembles telemetry wire frames out of the byte
 * stream. Every frame that passes its CRC goes to the recorder verbatim;
 * nothing here looks at the payload, so a packet type this firmware has
 * never heard of is still recorded.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/ring_buffer.h>

#include <aurora/lib/telemetry/wire.h>

#include "link.h"
#include "recorder.h"

LOG_MODULE_REGISTER(tlog_link, CONFIG_TELEMETRY_LOGGER_LOG_LEVEL);

/* The connector binding carries the phandle of whichever UART the board
 * wires to the data connector's TXD/RXD pins, so this file never names a
 * board-specific peripheral.
 */
#define STACK_CONNECTOR DT_CHOSEN(auxspace_stack_connector)

BUILD_ASSERT(DT_NODE_EXISTS(STACK_CONNECTOR),
	     "Set the auxspace,stack-connector chosen node to an "
	     "auxspaceev,aux-stack-data-connector node");
BUILD_ASSERT(DT_NODE_HAS_PROP(STACK_CONNECTOR, uart_bus),
	     "The chosen auxspace,stack-connector has no uart-bus property");

#define STACK_UART_NODE DT_PHANDLE(STACK_CONNECTOR, uart_bus)

BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(STACK_UART_NODE),
	     "The stack connector's uart-bus is disabled");

static const struct device *const stack_uart = DEVICE_DT_GET(STACK_UART_NODE);

static struct {
	atomic_t frames;
	atomic_t crc_errs;
	atomic_t overruns;
} stats;

RING_BUF_DECLARE(rx_rb, CONFIG_TELEMETRY_LOGGER_RX_RING_SIZE);
static K_SEM_DEFINE(rx_sem, 0, 1);

static void uart_isr(const struct device *dev, void *user_data)
{
	uint8_t buf[32];

	ARG_UNUSED(user_data);

	/* uart_irq_update() returns void here; it only latches the
	 * pending-interrupt state that the rx_ready query below reads.
	 */
	uart_irq_update(dev);

	while (uart_irq_rx_ready(dev)) {
		int n = uart_fifo_read(dev, buf, sizeof(buf));

		if (n <= 0) {
			break;
		}

		uint32_t put = ring_buf_put(&rx_rb, buf, (uint32_t)n);

		if (put < (uint32_t)n) {
			atomic_add(&stats.overruns, (uint32_t)n - put);
		}
		k_sem_give(&rx_sem);
	}
}

/* Frame reassembly state. Only touched by the parser thread.
 *
 * Same parser as abby's relay role, and with the same property: a
 * truncated frame leaves the parser mid-payload, so it eats a frame's
 * worth of the following bytes before it can look for a sync pair again,
 * and the frame after a truncation can be lost. abby writes whole frames
 * under a mutex, so in practice this only happens across a cable unplug.
 */
enum parse_state {
	P_MAGIC0 = 0,
	P_MAGIC1,
	P_TYPE,
	P_LEN,
	P_PAYLOAD,
	P_CRC_LO,
	P_CRC_HI,
};

static enum parse_state pstate;
static uint8_t frame[AURORA_TELEMETRY_WIRE_MAX_FRAME];
static uint16_t fidx;
static uint8_t payload_len;

static void frame_complete(void)
{
	uint16_t want = telemetry_wire_crc(frame, payload_len);
	uint16_t got = sys_get_le16(
		&frame[AURORA_TELEMETRY_WIRE_HDR_LEN + payload_len]);

	if (want != got) {
		atomic_inc(&stats.crc_errs);
		LOG_WRN("CRC mismatch (want %04x got %04x), resyncing",
			want, got);
		return;
	}

	atomic_inc(&stats.frames);
	LOG_DBG("frame type %u, %u byte payload", frame[2], payload_len);

	(void)recorder_submit(frame, fidx);
}

static void parse_byte(uint8_t b)
{
	switch (pstate) {
	case P_MAGIC0:
		if (b == AURORA_TELEMETRY_WIRE_MAGIC0) {
			frame[0] = b;
			pstate = P_MAGIC1;
		}
		break;

	case P_MAGIC1:
		if (b == AURORA_TELEMETRY_WIRE_MAGIC1) {
			frame[1] = b;
			pstate = P_TYPE;
		} else if (b != AURORA_TELEMETRY_WIRE_MAGIC0) {
			/* Not a sync pair; but a repeated magic0 could still
			 * be the real start, so only give up otherwise.
			 */
			pstate = P_MAGIC0;
		}
		break;

	case P_TYPE:
		frame[2] = b;
		pstate = P_LEN;
		break;

	case P_LEN:
		frame[3] = b;
		payload_len = b;
		fidx = AURORA_TELEMETRY_WIRE_HDR_LEN;
		pstate = (payload_len == 0) ? P_CRC_LO : P_PAYLOAD;
		break;

	case P_PAYLOAD:
		frame[fidx++] = b;
		if (fidx == (uint16_t)AURORA_TELEMETRY_WIRE_HDR_LEN +
				    payload_len) {
			pstate = P_CRC_LO;
		}
		break;

	case P_CRC_LO:
		frame[fidx++] = b;
		pstate = P_CRC_HI;
		break;

	case P_CRC_HI:
		frame[fidx++] = b;
		frame_complete();
		pstate = P_MAGIC0;
		break;
	}
}

static void link_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (1) {
		(void)k_sem_take(&rx_sem, K_FOREVER);

		uint8_t byte;

		while (ring_buf_get(&rx_rb, &byte, 1) == 1) {
			parse_byte(byte);
		}
	}
}

/* Started by link_init() rather than at boot, so no frame can reach the
 * recorder before recorder_init() has run.
 */
K_THREAD_DEFINE(tlog_link_tid, CONFIG_TELEMETRY_LOGGER_LINK_STACK_SIZE,
		link_thread, NULL, NULL, NULL,
		CONFIG_TELEMETRY_LOGGER_LINK_THREAD_PRIORITY, 0, SYS_FOREVER_MS);

void link_get_stats(struct link_stats *out)
{
	if (!out) {
		return;
	}
	out->frames = (uint32_t)atomic_get(&stats.frames);
	out->crc_errs = (uint32_t)atomic_get(&stats.crc_errs);
	out->overruns = (uint32_t)atomic_get(&stats.overruns);
}

int link_init(void)
{
	if (!device_is_ready(stack_uart)) {
		LOG_ERR("stack UART %s not ready", stack_uart->name);
		return -ENODEV;
	}

	k_thread_start(tlog_link_tid);

	uart_irq_callback_user_data_set(stack_uart, uart_isr, NULL);
	uart_irq_rx_enable(stack_uart);

	LOG_INF("stack link up on %s, listening for frames",
		stack_uart->name);

	return 0;
}
