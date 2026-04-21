/*
 * Copyright (c) 2025-2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_STATE_AUDIT_H_
#define APP_LIB_STATE_AUDIT_H_

#include <stdint.h>
#include <aurora/lib/state/state.h>

/**
 * @defgroup lib_state_audit State Machine Audit Log
 * @ingroup lib_state
 * @{
 *
 * @brief Ring-buffer based audit log for state machine transitions and events.
 *
 * Records timestamped entries for state transitions and notable events.
 * The log is a fixed-size ring buffer; oldest entries are silently
 * overwritten when the buffer is full.
 */

/** @brief Type of audit log entry. */
enum sm_audit_type {
	SM_AUDIT_TRANSITION,	/**< State transition. */
	SM_AUDIT_EVENT,		/**< Notable event (timeout, error, etc.). */
};

/** @brief Single audit log entry. */
struct sm_audit_entry {
	uint64_t timestamp_ns;		/**< Uptime in nanoseconds when recorded. */
	enum sm_audit_type type;	/**< Entry type. */
	enum sm_state from;		/**< Previous state (transitions only). */
	enum sm_state to;		/**< New state (transitions only). */
	const char *event;		/**< Short event description (events only). */
};

/**
 * @brief Record a state transition in the audit log.
 *
 * @param from Previous state.
 * @param to   New state.
 */
void sm_audit_transition(enum sm_state from, enum sm_state to);

/**
 * @brief Record a notable event in the audit log.
 *
 * @param state Current state when the event occurred.
 * @param event Short description of the event (must be a string literal
 *              or have static storage duration).
 */
void sm_audit_event(enum sm_state state, const char *event);

/**
 * @brief Get the number of entries currently stored in the audit log.
 *
 * @return Number of entries (up to CONFIG_AURORA_STATE_MACHINE_AUDIT_LOG_SIZE).
 */
uint32_t sm_audit_count(void);

/**
 * @brief Read an entry from the audit log.
 *
 * Index 0 is the oldest entry still in the buffer.
 *
 * @param idx   Index of the entry to read (0 = oldest).
 * @param entry Output pointer filled on success.
 *
 * @retval 0      Success.
 * @retval -EINVAL idx out of range or entry is NULL.
 */
int sm_audit_get(uint32_t idx, struct sm_audit_entry *entry);

/**
 * @brief Clear all entries from the audit log.
 */
void sm_audit_clear(void);

/** @} */

#endif /* APP_LIB_STATE_AUDIT_H_ */
