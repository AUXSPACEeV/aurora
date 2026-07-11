/**
 * @file data.h
 * @brief Flight-time logging glue for the sensor board state machine.
 *
 * Copyright (c) 2025-2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DATA_H
#define DATA_H

#include <aurora/lib/baro.h>
#include <aurora/lib/state/simple.h>

#if defined(CONFIG_DATA_LOGGER_BIN)
void log_handle_flight_lifecycle(const enum sm_state prev_state, const enum sm_state state);
void log_flight_telemetry(void);
void log_vbat_telemetry(void);


#else
static inline void log_handle_flight_lifecycle(const enum sm_state prev_state, const enum sm_state state) {}
static inline void log_flight_telemetry(void) {}
static inline void log_vbat_telemetry(void) {}
#endif /*CONFIG_DATA_LOGGER_BIN*/


#if defined(CONFIG_AURORA_PAD_LINK)
void update_pad_link_data(void);
#else
static inline void update_pad_link_data(void) {}
#endif /*CONFIG_AURORA_PAD_LINK*/

#endif
