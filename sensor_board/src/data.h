#ifndef LOGGER_H
#define LOGGER_H

#include <aurora/lib/baro.h>
#include "aurora/lib/state/simple.h"

#if defined(CONFIG_DATA_LOGGER_BIN)
void log_handle_flight_lifecycle(const enum sm_state prev_state, const enum sm_state state);
void log_flight_telemetry();

#else
static inline void log_handle_flight_lifecycle(const enum sm_state prev_state, const enum sm_state state);
static inline void log_flight_telemetry() {};
#endif

#endif
