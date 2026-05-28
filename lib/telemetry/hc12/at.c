/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT auxspaceev_hc12

#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "hc12_internal.h"

LOG_MODULE_DECLARE(telemetry_hc12, CONFIG_AURORA_TELEMETRY_LOG_LEVEL);

static const struct gpio_dt_spec set_gpio =
	GPIO_DT_SPEC_INST_GET_OR(0, set_gpios, {0});

/* Read bytes into resp until either resp_sz-1 is reached or no byte
 * arrives for quiet_ms. The HC-12 sends its whole AT response in one
 * burst, so an inactivity gap is the natural terminator.
 */
static size_t at_read(char *resp, size_t resp_sz, uint32_t quiet_ms)
{
	if (!resp || resp_sz == 0) {
		return 0;
	}

	int64_t last_byte_ms = k_uptime_get();
	size_t n = 0;
	uint8_t c;

	while (n + 1 < resp_sz) {
		if (uart_poll_in(hc12_uart_dev, &c) == 0) {
			resp[n++] = (char)c;
			last_byte_ms = k_uptime_get();
			continue;
		}
		if ((uint32_t)(k_uptime_get() - last_byte_ms) >= quiet_ms) {
			break;
		}
		k_msleep(2);
	}

	resp[n] = '\0';
	return n;
}

int hc12_at_exec(const char *cmd, size_t cmd_len,
		 char *resp, size_t resp_sz,
		 uint32_t quiet_ms,
		 size_t *resp_out_len)
{
	if (!set_gpio.port) {
		return -ENOTSUP;
	}
	if (!cmd || cmd_len == 0) {
		return -EINVAL;
	}

	int rc = 0;
	struct uart_config saved_cfg;
	bool cfg_saved = false;

	k_mutex_lock(&hc12_uart_lock, K_FOREVER);

	/* Save and switch host UART to 9600 baud for the AT exchange.
	 * uart_config_get may not be supported on every driver. If so
	 * we skip the restore step and the caller is responsible for
	 * matching DT current-speed to 9600.
	 */
	if (uart_config_get(hc12_uart_dev, &saved_cfg) == 0) {
		cfg_saved = true;
		if (saved_cfg.baudrate != HC12_AT_BAUD) {
			struct uart_config at_cfg = saved_cfg;
			at_cfg.baudrate = HC12_AT_BAUD;
			rc = uart_configure(hc12_uart_dev, &at_cfg);
			if (rc) {
				LOG_ERR("uart_configure(9600) failed (%d)", rc);
				rc = -EIO;
				goto out;
			}
		}
	}

	/* Drain any pending RX bytes from transparent-mode traffic so
	 * they don't get folded into our AT response.
	 */
	uint8_t junk;
	while (uart_poll_in(hc12_uart_dev, &junk) == 0) {
	}

	/* Enter AT mode. */
	(void)gpio_pin_set_dt(&set_gpio, 1);
	k_msleep(HC12_SET_SETTLE_MS);

	for (size_t i = 0; i < cmd_len; i++) {
		uart_poll_out(hc12_uart_dev, (uint8_t)cmd[i]);
	}

	size_t n = at_read(resp, resp_sz, quiet_ms);
	if (resp_out_len) {
		*resp_out_len = n;
	}

	/* Leave AT mode. */
	(void)gpio_pin_set_dt(&set_gpio, 0);
	k_msleep(HC12_SET_SETTLE_MS);

out:
	if (cfg_saved && saved_cfg.baudrate != HC12_AT_BAUD) {
		int rrc = uart_configure(hc12_uart_dev, &saved_cfg);
		if (rrc) {
			LOG_ERR("uart_configure(restore) failed (%d)", rrc);
			if (!rc) {
				rc = -EIO;
			}
		}
	}

	k_mutex_unlock(&hc12_uart_lock);
	return rc;
}
