/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_NOTIFY_H_
#define APP_LIB_NOTIFY_H_

#include <aurora/lib/state/simple.h>

/**
 * @defgroup lib_notify Notification library
 * @ingroup lib
 * @{
 *
 * @brief AURORA notification library for user-facing indicators.
 *
 * Provides an abstract notification interface backed by one or more
 * output devices (buzzer, RGB LED, …).  Each backend registers a
 * static @ref notify_backend and the library fans out every call to
 * all enabled backends.
 */

/**
 * @brief Notification backend operations.
 *
 * Each output device (buzzer, RGB LED, …) implements this vtable.
 */
struct notify_backend_api {
	/** @brief Called once at boot to initialise the backend. */
	int (*init)(void);

	/** @brief Signal that the system has booted. */
	int (*on_boot)(void);

	/** @brief Signal a flight state-machine transition. */
	int (*on_state_change)(enum sm_state prev, enum sm_state next);

	/** @brief Signal an error condition. */
	int (*on_error)(void);

	/** @brief Signal a power failure (or @p recover from a powerfail) */
	void (*on_powerfail)(int recover);
};

/**
 * @brief Notification backend descriptor.
 *
 * Backends are collected automatically at link time via an iterable
 * section; no central registration call is needed.
 */
struct notify_backend {
	const struct notify_backend_api *api;
};

/**
 * @brief Register a notification backend (link-time).
 *
 * Place a @ref notify_backend instance in the iterable section so the
 * notification library discovers it without explicit registration.
 *
 * @param _name  Unique C identifier for this backend.
 * @param _api   Pointer to a @ref notify_backend_api vtable.
 */
#define NOTIFY_BACKEND_DEFINE(_name, _api)			\
	STRUCT_SECTION_ITERABLE(notify_backend, _name) = {	\
		.api = (_api),					\
	}

/**
 * @brief Initialise all notification backends.
 *
 * @retval 0 on success, or first non-zero return from a backend.
 */
int notify_init(void);

/**
 * @brief Notify all backends that the system has booted.
 *
 * @retval 0 on success, or first non-zero return from a backend.
 */
int notify_boot(void);

/**
 * @brief Notify all backends of a state-machine transition.
 *
 * @param prev Previous state.
 * @param next New state.
 *
 * @retval 0 on success, or first non-zero return from a backend.
 */
int notify_state_change(enum sm_state prev, enum sm_state next);

/**
 * @brief Notify all backends about a powerfail
 *
 * @param recover 1 if Power failure is being recovered.
 */
void notify_powerfail(int recover);

/**
 * @brief Notify all backends of an error condition.
 *
 * @retval 0 on success, or first non-zero return from a backend.
 */
int notify_error(void);

/** @} */

#endif /* APP_LIB_NOTIFY_H_ */
