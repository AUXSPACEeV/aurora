/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 *
 * HC-12 telemetry backend: the uart-link engine plus the two things
 * that are actually about the HC-12.
 *
 *   - A SET pin, which drops the module into AT-command mode. When it
 *     is not wired the module is bench-provisioned and runtime
 *     reconfiguration is simply unavailable; the backend still works.
 *   - A UART lock, because the AT helper reconfigures the line to 9600
 *     baud for the duration of a command exchange. The engine holds
 *     the same lock around every frame so the two cannot interleave.
 *
 * Framing, payload packing, rate limiting and the transmit worker are
 * not HC-12 concerns and live in lib/telemetry/uart_link.
 */

#define DT_DRV_COMPAT auxspaceev_hc12

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <aurora/lib/telemetry.h>
#include <aurora/lib/telemetry/uart_link.h>

#include "hc12_internal.h"

LOG_MODULE_REGISTER(telemetry_hc12, CONFIG_AURORA_TELEMETRY_LOG_LEVEL);

BUILD_ASSERT(DT_HAS_COMPAT_STATUS_OKAY(auxspaceev_hc12),
	     "An auxspaceev,hc12 node must be enabled in devicetree");

const struct device *const hc12_uart_dev =
	DEVICE_DT_GET(DT_INST_PHANDLE(0, uart));

/* set_gpio.port == NULL when SET is not wired in DT: runtime AT is
 * unavailable in that case (the shell command refuses, init still
 * succeeds).
 */
static const struct gpio_dt_spec set_gpio =
	GPIO_DT_SPEC_INST_GET_OR(0, set_gpios, {0});

K_MUTEX_DEFINE(hc12_uart_lock);

K_MSGQ_DEFINE(hc12_txq, sizeof(struct uart_link_frame),
	      CONFIG_AURORA_TELEMETRY_HC12_QUEUE_DEPTH, 4);

static struct uart_link link = {
	.uart = DEVICE_DT_GET(DT_INST_PHANDLE(0, uart)),
	.txq = &hc12_txq,
	.lock = &hc12_uart_lock,
	.min_interval_ms = CONFIG_AURORA_TELEMETRY_HC12_MIN_INTERVAL_MS,
	.name = "HC-12",
};

static int hc12_send_sm_update(enum sm_state state, enum sm_type type,
			       const struct sm_inputs *inputs)
{
	return uart_link_send_sm_update(&link, state, type, inputs);
}

static void hc12_tx_task(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	uart_link_tx_worker(&link);
}

K_THREAD_DEFINE(hc12_tx, CONFIG_AURORA_TELEMETRY_HC12_STACK_SIZE,
		hc12_tx_task, NULL, NULL, NULL,
		CONFIG_AURORA_TELEMETRY_HC12_THREAD_PRIORITY, 0, 0);

static int hc12_init(void)
{
	/* Before arming the link, so no frame can go out while SET is
	 * still floating.
	 */
	if (set_gpio.port) {
		if (!gpio_is_ready_dt(&set_gpio)) {
			LOG_ERR("SET GPIO not ready");
			return -ENODEV;
		}
		/* Inactive (the binding flags ACTIVE_LOW, so "inactive"
		 * means transparent mode).
		 */
		int rc = gpio_pin_configure_dt(&set_gpio,
					       GPIO_OUTPUT_INACTIVE);
		if (rc) {
			LOG_ERR("SET GPIO configure failed (%d)", rc);
			return rc;
		}
	} else {
		LOG_INF("HC-12 SET pin not wired: runtime AT disabled");
	}

	return uart_link_init(&link);
}

static const struct telemetry_backend_api hc12_api = {
	.init           = hc12_init,
	.send_sm_update = hc12_send_sm_update,
};

TELEMETRY_BACKEND_DEFINE(hc12, &hc12_api);
