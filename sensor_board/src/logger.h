#ifndef LOGGER_H
#define LOGGER_H

#include "aurora/lib/imu.h"
#include "aurora/lib/data_logger.h"
#include "aurora/lib/state/simple.h"
#include <aurora/lib/baro.h>

#if defined(CONFIG_DATA_LOGGER_BIN)
void log_imu_data(const struct imu_data *imu);
void log_handle_flight_lifecycle(const enum sm_state prev_state, const enum sm_state state);
void log_baro_data(const struct baro_data *baro);
void log_flight_telemetry();

#else
static inline void log_imu(const struct imu_data *imu) {};
static inline void log_better_name(const enum sm_state prev_state, const enum sm_state state) {};
static inline void log_baro_data(const struct baro_data *baro) {};
static inline void log_flight_telemetry() {};
#endif

#endif