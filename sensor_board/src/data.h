/**
 * @file data.h
 * @brief Flight-time logging glue for the sensor board state machine.
 *
 * Copyright (c) 2025-2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DATA_H
#define DATA_H

#include <stdbool.h>

#include <aurora/lib/baro.h>
#include <aurora/lib/state/state.h>

#if defined(CONFIG_DATA_LOGGER_BIN)
void log_handle_flight_lifecycle(const enum sm_state prev_state, const enum sm_state state);
void log_flight_telemetry(void);
void log_vbat_telemetry(void);

/**
 * @brief Open a flight log for a state restored after a watchdog reset.
 *
 * The recovery path writes the state machine's state directly rather than
 * transitioning into it, so the state machine task never sees the
 * IDLE->ARMED edge that @ref log_handle_flight_lifecycle keys the recorder
 * off.  Without this a recovered flight is armed but records nothing, for
 * its whole duration.
 *
 * Replaying the full lifecycle handler instead is not safe: its LANDED
 * branch schedules a close for a log that was never opened.  This covers
 * only the airborne states, where opening a fresh log is the right answer.
 *
 * @param state State the machine resumed in.
 */
void log_resume_flight_after_reset(const enum sm_state state);
#else
static inline void log_handle_flight_lifecycle(const enum sm_state prev_state, const enum sm_state state) {}
static inline void log_flight_telemetry(void) {}
static inline void log_vbat_telemetry(void) {}
static inline void log_resume_flight_after_reset(const enum sm_state state) {}
#endif /* CONFIG_DATA_LOGGER_BIN */

#if defined(CONFIG_AURORA_PAD_LINK)
void update_pad_link_data(void);
#else
static inline void update_pad_link_data(void) {}
#endif /*CONFIG_AURORA_PAD_LINK*/

/**
 * @brief Arming precondition: is flight-time data logging available?
 *
 * Feeds @ref sm_inputs.log_ready so the state machine refuses to leave IDLE
 * for ARMED and drops back from ARMED to IDLE pre-boost when the flight
 * cannot be recorded.  False when the boot-time disk bring-up found the raw
 * flight-log region unusable (with CONFIG_DATA_LOGGER_DISK_AUTO_MKFS), or
 * once opening the flight log at ARM time has failed (latched until
 * reboot).  Builds without the binary logger have nothing to gate on and
 * always report ready.
 */
#if defined(CONFIG_DATA_LOGGER_BIN)
bool log_flight_log_online(void);
#else
static inline bool log_flight_log_online(void) { return true; }
#endif /* CONFIG_DATA_LOGGER_BIN */

#endif /* DATA_H */
