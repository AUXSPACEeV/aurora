/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The AUX-Stack data-connector UART: the wire to the brain board.
 *
 * Which direction it runs depends on the role.
 *
 *   relay   - drains the UART and reassembles telemetry wire frames out
 *             of the byte stream, handing each one to the radio.
 *   receive - writes frames that arrived over the air back out as the
 *             same byte stream, so whatever is attached sees exactly
 *             what a directly wired sender would have produced.
 *
 * Both directions are payload-agnostic: the magic pair and the CRC are
 * validated, and the frame then travels on verbatim, so a new packet
 * type reaches its destination without this relay having to know
 * anything about it.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/ring_buffer.h>

#include <aurora/lib/telemetry/wire.h>

#include "downlink.h"
#include "link.h"

LOG_MODULE_REGISTER(abby_link, CONFIG_ABBY_LOG_LEVEL);

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
	atomic_t sent;
} stats;

/* Stack-link activity LED. It means the same thing in both roles: a
 * frame just crossed the connector. downlink.c owns led0 for the radio.
 */
#if DT_NODE_EXISTS(DT_ALIAS(led1))
static const struct gpio_dt_spec link_led =
	GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

static void link_led_off(struct k_work *work)
{
	ARG_UNUSED(work);
	(void)gpio_pin_set_dt(&link_led, 0);
}

static K_WORK_DELAYABLE_DEFINE(link_led_work, link_led_off);

static void link_led_pulse(void)
{
	if (!link_led.port) {
		return;
	}
	(void)gpio_pin_set_dt(&link_led, 1);
	(void)k_work_reschedule(&link_led_work,
				K_MSEC(CONFIG_ABBY_LED_PULSE_MS));
}
#else
static void link_led_pulse(void) { }
#endif

#if defined(CONFIG_ABBY_ROLE_RELAY)

RING_BUF_DECLARE(rx_rb, CONFIG_ABBY_LINK_RX_RING_SIZE);
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
 * Resynchronisation is magic-pair plus CRC, with one consequence worth
 * knowing: a truncated frame leaves the parser mid-payload, so it eats a
 * frame's worth of the following bytes before it can look for a sync pair
 * again, and the frame after a truncation can be lost. That is inherent to
 * this style of framing rather than a defect, and it costs nothing here -
 * the sender writes whole frames under a mutex, and the duty-cycle budget
 * puts seconds between them.
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
	link_led_pulse();

	int rc = downlink_submit(frame, fidx);

	if (rc == -EMSGSIZE) {
		LOG_WRN("frame of %u bytes exceeds one LoRa packet", fidx);
	}
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

K_THREAD_DEFINE(abby_link_tid, CONFIG_ABBY_LINK_STACK_SIZE,
		link_thread, NULL, NULL, NULL,
		CONFIG_ABBY_LINK_THREAD_PRIORITY, 0, 0);

#else /* CONFIG_ABBY_ROLE_RECEIVE */

static K_MUTEX_DEFINE(tx_lock);

int link_send(const uint8_t *frame, size_t len)
{
	if (!frame || len == 0) {
		return -EINVAL;
	}

	/* uart_poll_out() busy-waits a character at a time, so the lock is
	 * held for the whole frame rather than per byte: a frame
	 * interleaved with another is unparseable at the far end, where a
	 * late one is merely late. The radio worker and the shell's test
	 * command are the two callers that can collide here.
	 */
	k_mutex_lock(&tx_lock, K_FOREVER);
	for (size_t i = 0; i < len; i++) {
		uart_poll_out(stack_uart, frame[i]);
	}
	k_mutex_unlock(&tx_lock);

	atomic_inc(&stats.sent);
	link_led_pulse();

	LOG_DBG("wrote %u bytes to %s", (unsigned int)len, stack_uart->name);
	return 0;
}

#endif /* CONFIG_ABBY_ROLE_* */

void link_get_stats(struct link_stats *out)
{
	if (!out) {
		return;
	}
	out->frames = (uint32_t)atomic_get(&stats.frames);
	out->crc_errs = (uint32_t)atomic_get(&stats.crc_errs);
	out->overruns = (uint32_t)atomic_get(&stats.overruns);
	out->sent = (uint32_t)atomic_get(&stats.sent);
}

int link_init(void)
{
	if (!device_is_ready(stack_uart)) {
		LOG_ERR("stack UART %s not ready", stack_uart->name);
		return -ENODEV;
	}

#if DT_NODE_EXISTS(DT_ALIAS(led1))
	if (link_led.port && gpio_is_ready_dt(&link_led)) {
		(void)gpio_pin_configure_dt(&link_led, GPIO_OUTPUT_INACTIVE);
	}
#endif

#if defined(CONFIG_ABBY_ROLE_RELAY)
	uart_irq_callback_user_data_set(stack_uart, uart_isr, NULL);
	uart_irq_rx_enable(stack_uart);

	LOG_INF("stack link up on %s, listening for frames",
		stack_uart->name);
#else
	LOG_INF("stack link up on %s, writing received frames out",
		stack_uart->name);
#endif

	return 0;
}
