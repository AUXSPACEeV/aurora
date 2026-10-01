/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Telemetry backend for a plain UART: the uart-link engine with
 * nothing bolted on.
 *
 * This is what a brain board uses to feed a peer that already speaks
 * the telemetry wire format - an AUX-Tel relay on the stack data
 * connector, or a laptop on a USB-serial adapter. There is no
 * provisioning pin and no command set, because the far end needs
 * neither.
 */

#define DT_DRV_COMPAT auxspaceev_telemetry_uart_link

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <aurora/lib/telemetry.h>
#include <aurora/lib/telemetry/uart_link.h>

LOG_MODULE_DECLARE(telemetry_uart_link, CONFIG_AURORA_TELEMETRY_LOG_LEVEL);

BUILD_ASSERT(DT_HAS_COMPAT_STATUS_OKAY(auxspaceev_telemetry_uart_link),
	     "An auxspaceev,telemetry-uart-link node must be enabled in "
	     "devicetree");

K_MSGQ_DEFINE(uart_link_txq, sizeof(struct uart_link_frame),
	      CONFIG_AURORA_TELEMETRY_UART_LINK_QUEUE_DEPTH, 4);

/* No lock: nothing else drives this UART. */
static struct uart_link link = {
	.uart = DEVICE_DT_GET(DT_INST_PHANDLE(0, uart)),
	.txq = &uart_link_txq,
	.lock = NULL,
	.min_interval_ms = CONFIG_AURORA_TELEMETRY_UART_LINK_MIN_INTERVAL_MS,
	.name = "uart-link",
};

static int uart_link_backend_send(enum sm_state state, enum sm_type type,
				  const struct sm_inputs *inputs)
{
	return uart_link_send_sm_update(&link, state, type, inputs);
}

static int uart_link_backend_send_status(const struct telemetry_status *status)
{
	return uart_link_send_status(&link, status);
}

static void uart_link_tx_task(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	uart_link_tx_worker(&link);
}

K_THREAD_DEFINE(uart_link_tx, CONFIG_AURORA_TELEMETRY_UART_LINK_STACK_SIZE,
		uart_link_tx_task, NULL, NULL, NULL,
		CONFIG_AURORA_TELEMETRY_UART_LINK_THREAD_PRIORITY, 0, 0);

static int uart_link_backend_init(void)
{
	return uart_link_init(&link);
}

static const struct telemetry_backend_api uart_link_api = {
	.init           = uart_link_backend_init,
	.send_sm_update = uart_link_backend_send,
	.send_status    = uart_link_backend_send_status,
};

TELEMETRY_BACKEND_DEFINE(uart_link, &uart_link_api);
