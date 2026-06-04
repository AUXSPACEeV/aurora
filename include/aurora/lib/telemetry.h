/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AURORA_LIB_TELEMETRY_H_
#define AURORA_LIB_TELEMETRY_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/sys/iterable_sections.h>

#include <aurora/lib/state/state.h>

/**
 * @defgroup lib_telemetry Telemetry library
 * @ingroup lib
 * @{
 *
 * @brief Downlink of flight data over one or more pluggable transport
 *        backends (HC-12, LoRaWAN, …).
 *
 * The core is a small dispatcher: every backend that registers via
 * @ref TELEMETRY_BACKEND_DEFINE is initialised on boot and receives
 * each outgoing message. Backends own their own framing, transport,
 * worker threads, and any backend-specific rate limiting.
 */

/**
 * @brief Backend operations vtable.
 *
 * All members are optional; NULL slots are skipped by the dispatcher.
 */
struct telemetry_backend_api {
	/** @brief Called once from telemetry_init(). */
	int (*init)(void);

	/** @brief Deliver a state-machine update. Must not block.
	 *
	 * @c type identifies the @c sm_state enum mapping in use
	 * (see @ref sm_get_type); backends forward it so the receiver
	 * can decode the @c state value without prior agreement.
	 */
	int (*send_sm_update)(enum sm_state state, enum sm_type type,
			      const struct sm_inputs *inputs);
};

/** @brief Backend descriptor. Collected at link time. */
struct telemetry_backend {
	const char *name;
	const struct telemetry_backend_api *api;
};

/**
 * @brief Register a telemetry backend (link-time).
 *
 * @param _name  Unique C identifier for this backend.
 * @param _api   Pointer to a @ref telemetry_backend_api vtable.
 */
#define TELEMETRY_BACKEND_DEFINE(_name, _api)				\
	STRUCT_SECTION_ITERABLE(telemetry_backend, _name) = {		\
		.name = #_name,						\
		.api = (_api),						\
	}

/**
 * @brief Initialise all registered telemetry backends.
 *
 * @retval 0 on success, or the first non-zero return from a backend.
 */
int telemetry_init(void);

/**
 * @brief Fan out a state-machine update to all registered backends.
 *
 * Safe to call from any thread context. Never blocks — backends must
 * enforce that themselves (typically by dropping on overflow).
 *
 * @param state   Current flight state.
 * @param type    Active state machine implementation ID
 *                (see @ref sm_get_type). Forwarded so the receiver
 *                can decode @p state without prior agreement.
 * @param inputs  Current SM inputs snapshot (see sm_get_inputs).
 *
 * @retval 0 if every backend accepted the message.
 * @retval <0 the first error returned by any backend (others still tried).
 */
int telemetry_send_sm_update(enum sm_state state, enum sm_type type,
			     const struct sm_inputs *inputs);

/** @} */

#endif /* AURORA_LIB_TELEMETRY_H_ */
