/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The transport engine shared by every UART telemetry backend. See
 * <aurora/lib/telemetry/uart_link.h> for what belongs here and what
 * belongs in a backend.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <aurora/lib/telemetry/uart_link.h>
#include <aurora/lib/telemetry/wire.h>

LOG_MODULE_REGISTER(telemetry_uart_link, CONFIG_AURORA_TELEMETRY_LOG_LEVEL);

/* Refuse if the previous accepted message is still inside the rate
 * window. The timestamp advances on acceptance only, so a refused call
 * does not extend the window.
 */
static int rate_limit(struct uart_link *link)
{
	if (link->min_interval_ms <= 0) {
		return 0;
	}

	int64_t now_ms = k_uptime_get();
	k_spinlock_key_t key = k_spin_lock(&link->rl_lock);

	if (now_ms - link->last_send_ms < link->min_interval_ms) {
		k_spin_unlock(&link->rl_lock, key);
		return -EAGAIN;
	}

	link->last_send_ms = now_ms;
	k_spin_unlock(&link->rl_lock, key);

	return 0;
}

int uart_link_send_sm_update(struct uart_link *link, enum sm_state state,
			     enum sm_type type,
			     const struct sm_inputs *inputs)
{
	if (!link || !inputs) {
		return -EINVAL;
	}

	if (!atomic_get(&link->ready)) {
		return -ENODEV;
	}

	int rc = rate_limit(link);

	if (rc) {
		return rc;
	}

	struct telemetry_wire_sm_update p = {
		.timestamp_ms = (uint32_t)k_uptime_get(),
		.state        = (uint8_t)state,
		.armed        = inputs->armed,
		.sm_type      = (uint8_t)type,
		.altitude     = inputs->altitude,
		.acceleration = inputs->acceleration,
		.accel_vert   = inputs->accel_vert,
		.velocity     = inputs->velocity,
		.orientation  = {
			inputs->orientation[0],
			inputs->orientation[1],
			inputs->orientation[2],
		},
	};

	struct uart_link_frame f;
	size_t n = telemetry_wire_finalise(f.buf, sizeof(f.buf),
					   AURORA_TELEMETRY_WIRE_TYPE_SM_UPDATE,
					   &p, (uint8_t)sizeof(p));

	if (n == 0) {
		/* Unreachable: uart_link_frame is sized from this payload. */
		return -ENOMEM;
	}
	f.len = (uint8_t)n;

	if (k_msgq_put(link->txq, &f, K_NO_WAIT) != 0) {
		return -ENOMEM;
	}

	return 0;
}

BUILD_ASSERT(sizeof(struct telemetry_wire_status) <=
		     sizeof(struct telemetry_wire_sm_update),
	     "uart_link_frame is sized for SM_UPDATE; grow it for STATUS");

int uart_link_send_status(struct uart_link *link,
			  const struct telemetry_status *status)
{
	if (!link || !status) {
		return -EINVAL;
	}

	if (!atomic_get(&link->ready)) {
		return -ENODEV;
	}

	/* No rate_limit(): the window is shared with SM updates, which
	 * would then starve the heartbeat on a link that is otherwise
	 * busy. The caller already sends these at a low fixed rate.
	 */
	uint8_t flags = 0;

	flags |= status->armed ? AURORA_TELEMETRY_WIRE_STATUS_ARMED : 0;
	flags |= status->imu_ok ? AURORA_TELEMETRY_WIRE_STATUS_IMU_OK : 0;
	flags |= status->baro_ok ? AURORA_TELEMETRY_WIRE_STATUS_BARO_OK : 0;
	flags |= status->calibrated ?
		AURORA_TELEMETRY_WIRE_STATUS_CALIBRATED : 0;
	flags |= status->log_ready ?
		AURORA_TELEMETRY_WIRE_STATUS_LOG_READY : 0;

	struct telemetry_wire_status p = {
		.timestamp_ms = (uint32_t)k_uptime_get(),
		.state        = (uint8_t)status->state,
		.sm_type      = (uint8_t)status->type,
		.flags        = flags,
	};

	struct uart_link_frame f;
	size_t n = telemetry_wire_finalise(f.buf, sizeof(f.buf),
					   AURORA_TELEMETRY_WIRE_TYPE_STATUS,
					   &p, (uint8_t)sizeof(p));

	if (n == 0) {
		/* Unreachable: see the BUILD_ASSERT above. */
		return -ENOMEM;
	}
	f.len = (uint8_t)n;

	if (k_msgq_put(link->txq, &f, K_NO_WAIT) != 0) {
		return -ENOMEM;
	}

	return 0;
}

FUNC_NORETURN void uart_link_tx_worker(struct uart_link *link)
{
	struct uart_link_frame f;

	while (1) {
		(void)k_msgq_get(link->txq, &f, K_FOREVER);

		/* Hold the lock for the whole frame where one is
		 * configured: an out-of-band user may have reconfigured
		 * the line, and interleaving would garble both sides.
		 */
		if (link->lock) {
			k_mutex_lock(link->lock, K_FOREVER);
		}

		for (uint8_t i = 0; i < f.len; i++) {
			uart_poll_out(link->uart, f.buf[i]);
		}

		if (link->lock) {
			k_mutex_unlock(link->lock);
		}
	}
}

int uart_link_init(struct uart_link *link)
{
	if (!link || !link->uart || !link->txq) {
		return -EINVAL;
	}

	if (!device_is_ready(link->uart)) {
		LOG_ERR("%s: UART %s not ready", link->name,
			link->uart->name);
		return -ENODEV;
	}

	atomic_set(&link->ready, 1);
	LOG_INF("%s up on %s", link->name, link->uart->name);

	return 0;
}
