/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AURORA_LIB_TELEMETRY_UART_LINK_H_
#define AURORA_LIB_TELEMETRY_UART_LINK_H_

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/toolchain.h>

#include <aurora/lib/state/state.h>
#include <aurora/lib/telemetry.h>
#include <aurora/lib/telemetry/wire.h>

/**
 * @defgroup lib_telemetry_uart_link Telemetry UART link
 * @ingroup lib_telemetry
 * @{
 *
 * @brief The transport engine behind every UART-based telemetry
 *        backend.
 *
 * Writing telemetry frames to a UART is the same job whether the far
 * end is an HC-12 radio bridge, an AUX-Tel relay on the stack
 * connector, or a laptop on a USB-serial adapter: pack the payload,
 * frame it, rate-limit it, and push the bytes out from a worker thread
 * so the caller never blocks. That job lives here.
 *
 * What differs per far end - a provisioning pin, an AT command set, a
 * modem that needs its baud rate changed - stays in the backend that
 * owns it. The HC-12 backend is this engine plus a SET pin and an AT
 * helper.
 *
 * A backend supplies one @ref uart_link instance, its own message
 * queue and its own thread, then wires three one-line calls into a
 * @ref telemetry_backend_api. Instances are independent, so several
 * can run at once on different UARTs with different rate limits.
 */

/** @brief A queued frame, sized for the largest payload the engine builds. */
struct uart_link_frame {
	uint8_t len;
	uint8_t buf[AURORA_TELEMETRY_WIRE_OVERHEAD +
		    sizeof(struct telemetry_wire_sm_update)];
};

/**
 * @brief One UART telemetry link.
 *
 * The first group is configuration, filled in by the backend at
 * compile time. The rest is engine state; leave it zeroed.
 */
struct uart_link {
	/** UART the frames go out on. */
	const struct device *uart;

	/** Queue between the producer and the transmit worker. Entries
	 *  must be @c sizeof(struct uart_link_frame).
	 */
	struct k_msgq *txq;

	/** Optional: held for the duration of each frame. Needed only
	 *  when something else also drives this UART out of band - the
	 *  HC-12 AT helper reconfigures the line and toggles SET, and a
	 *  frame written across that would garble both. NULL when the
	 *  link owns the UART outright.
	 */
	struct k_mutex *lock;

	/** Lower bound between accepted messages, in ms. Calls arriving
	 *  sooner are refused with @c -EAGAIN and dropped. 0 disables
	 *  the limit.
	 */
	int32_t min_interval_ms;

	/** Name used in log lines. */
	const char *name;

	/* --- engine state --- */
	atomic_t ready;
	int64_t last_send_ms;
	struct k_spinlock rl_lock;
};

/**
 * @brief Check the UART and arm the link.
 *
 * Until this succeeds, @ref uart_link_send_sm_update refuses with
 * @c -ENODEV.
 *
 * @param link Link instance.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p link is incompletely configured.
 * @retval -ENODEV if the UART is not ready.
 */
int uart_link_init(struct uart_link *link);

/**
 * @brief Frame a state-machine update and queue it for transmission.
 *
 * Never blocks: a full queue drops the frame rather than stalling the
 * caller, which may be the flight state machine.
 *
 * @param link   Link instance.
 * @param state  Current flight state.
 * @param type   Active state machine implementation ID.
 * @param inputs Current SM inputs snapshot.
 *
 * @retval 0 on success.
 * @retval -ENODEV if the link is not initialised.
 * @retval -EAGAIN if refused by the rate limiter.
 * @retval -ENOMEM if the transmit queue is full.
 */
int uart_link_send_sm_update(struct uart_link *link, enum sm_state state,
			     enum sm_type type,
			     const struct sm_inputs *inputs);

/**
 * @brief Frame a STATUS heartbeat and queue it for transmission.
 *
 * Never blocks. Not subject to @ref uart_link.min_interval_ms, which
 * governs SM updates only: the caller paces heartbeats itself.
 *
 * @param link   Link instance.
 * @param status Health snapshot to send.
 *
 * @retval 0 on success.
 * @retval -EINVAL if an argument is NULL.
 * @retval -ENODEV if the link is not initialised.
 * @retval -ENOMEM if the transmit queue is full.
 */
int uart_link_send_status(struct uart_link *link,
			  const struct telemetry_status *status);

/**
 * @brief Transmit worker body. Does not return.
 *
 * Call this as the entry point of a thread the backend defines, so it
 * can size the stack and set the priority itself.
 *
 * @param link Link instance.
 */
FUNC_NORETURN void uart_link_tx_worker(struct uart_link *link);

/** @} */

#endif /* AURORA_LIB_TELEMETRY_UART_LINK_H_ */
