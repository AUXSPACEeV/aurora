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
void update_pad_link_data(void);


#else
static inline void log_handle_flight_lifecycle(const enum sm_state prev_state, const enum sm_state state) {}
static inline void log_flight_telemetry(void) {}
static inline void log_vbat_telemetry(void) {}
static inline void update_pad_link_data(void) {}
#endif

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
#endif

#endif
