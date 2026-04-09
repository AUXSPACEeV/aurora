/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef AURORA_LIB_DATA_LOGGER_H_
#define AURORA_LIB_DATA_LOGGER_H_

#include <stddef.h>
#include <zephyr/drivers/sensor.h>

/**
 * @defgroup lib_data_logger Data Logger
 * @ingroup lib
 * @{
 *
 * @brief Format-agnostic telemetry data logger for flight/sensor applications.
 *
 * Sensor readings are captured as @ref datapoint and serialised through a
 * pluggable formatter backend (vtable pattern).  The public API is identical
 * regardless of the chosen output format.
 *
 * Typical usage:
 * @code
 *   static struct data_logger logger;
 *   data_logger_init(&logger, "flight");
 *   data_logger_log(&dp);
 *   data_logger_flush(&logger);
 *   data_logger_close(&logger);
 * @endcode
 */

/** Maximum number of sensor channels carried by a single datapoint. */
#define DP_MAX_CHANNELS 3

/**
 * @brief Sensor group identifier.
 *
 * Always add new entries **before** @c AURORA_DATA_COUNT so that the count
 * stays accurate and array-based tables remain valid.
 */
enum aurora_data {
	AURORA_DATA_BARO,       /**< Barometer: [0] temperature, [1] pressure */
	AURORA_DATA_IMU_ACCEL,  /**< Accelerometer: [0] x, [1] y, [2] z       */
	AURORA_DATA_IMU_GYRO,   /**< Gyroscope:     [0] x, [1] y, [2] z       */
	AURORA_DATA_IMU_MAG,    /**< Magnetometer:  [0] x, [1] y, [2]  z      */
	AURORA_DATA_COUNT,      /**< Sentinel — do not use as a type          */
};

/**
 * @brief Flat telemetry data point.
 *
 * Carries a timestamp, sensor-group type, the number of valid channels, and
 * up to @ref DP_MAX_CHANNELS raw Zephyr @c sensor_value readings.
 */
struct datapoint {
	int64_t timestamp_ms;                          /**< ms since launch           */
	enum aurora_data type;                         /**< Sensor group              */
	uint8_t channel_count;                         /**< Valid entries in channels */
	struct sensor_value channels[DP_MAX_CHANNELS]; /**< Channel readings          */
};

/** Forward declaration (needed by formatter callbacks). */
struct data_logger;

/**
 * @brief Formatter vtable.
 *
 * Implement all mandatory callbacks and export a
 * @c const @c struct @c data_logger_formatter to create a new backend.
 *
 * Every callback receives the logger instance so it can reach its opaque
 * context via @c logger->ctx.  Return 0 on success, negative errno on
 * failure.
 */
struct data_logger_formatter {
	/** Open the output file and allocate formatter context into logger->ctx. */
	int (*init)(struct data_logger *logger, const char *path);

	/** Write the format-specific preamble (e.g. CSV header row). */
	int (*write_header)(struct data_logger *logger);

	/** Serialise one datapoint to the output. */
	int (*write_datapoint)(struct data_logger *logger,
			       const struct datapoint *dp);

	/** Flush buffered data to storage. */
	int (*flush)(struct data_logger *logger);

	/** Finalise the file and free the formatter context. */
	int (*close)(struct data_logger *logger);

	/** Optional preparation before logging stop and fsync. */
	int (*stop)(struct data_logger *logger);

	/** Optional preparation to start logging. */
	int (*start)(struct data_logger *logger);

	/** File suffix */
	char file_ext[8];

	/** Human-readable formatter name (e.g. "csv", "influx", "mock") */
	char name[8];
};

/**
 * @brief Formatter state.
 */
struct data_logger_state {
	/** Mutex for thread safety in concurrent access */
	struct k_mutex mutex;

	/** Data logger is running and logging */
	int running;
};

/** Maximum length of a data logger name (including NUL). */
#define DATA_LOGGER_NAME_MAX 16

/**
 * @brief Logger instance (caller-allocated, typically stack or static).
 */
struct data_logger {
	const struct data_logger_formatter *fmt; /**< Active formatter vtable  */
	struct data_logger_state *state;         /**< Active formatter state */
	void *ctx;                               /**< Opaque formatter context */
	char name[DATA_LOGGER_NAME_MAX];        /**< Logger name (set by init) */
};

/**
 * @brief Initialise a logger.
 *
 * The output file is placed at
 * @c CONFIG_DATA_LOGGER_BASE_PATH/[ @p filename ].file_ext, where file_ext
 * comes from the active formatter.  Calls @c fmt->init then
 * @c fmt->write_header.  On any failure the logger is left in an invalid
 * state and should not be used.
 *
 * @param logger    Caller-allocated logger instance.
 * @param filename  Base filename (without extension).
 * @retval 0 on success, negative errno on failure.
 */
int data_logger_init(struct data_logger *logger, const char *filename);

/**
 * @brief Set the default logger used by @ref data_logger_log.
 *
 * @param logger  Initialised logger instance (NULL to clear).
 */
void data_logger_set_default(struct data_logger *logger);

/**
 * @brief Log a datapoint to the default logger.
 *
 * Uses the logger previously registered with @ref data_logger_set_default.
 * Returns -ENODEV if no default logger has been set.
 *
 * @param dp  Datapoint to write.
 * @retval 0 on success, negative errno on failure.
 */
int data_logger_log(const struct datapoint *dp);

/**
 * @brief Serialise and store one datapoint to a specific logger.
 *
 * @param logger  Initialised logger instance.
 * @param dp      Datapoint to write.
 * @retval 0 on success, negative errno on failure.
 */
int data_logger_write(struct data_logger *logger, const struct datapoint *dp);

/**
 * @brief Flush buffered data to the underlying storage.
 *
 * @param logger  Initialised logger instance.
 * @retval 0 on success, negative errno on failure.
 */
int data_logger_flush(struct data_logger *logger);

/**
 * @brief Finalise the output file and release formatter resources.
 *
 * After this call @p logger is reset and must be re-initialised before use.
 *
 * @param logger  Initialised logger instance.
 * @retval 0 on success, negative errno on failure.
 */
int data_logger_close(struct data_logger *logger);

/**
 * @brief Temporary stop the data logger and flush the fs caches.
 *
 * After this call @p logger is stopped and must be re-started before use.
 *
 * @param logger  Initialised logger instance.
 * @retval 0 on success, negative errno on failure.
 */
int data_logger_stop(struct data_logger *logger);

/**
 * @brief Restart the data logger.
 *
 * After this call @p logger is started and running.
 *
 * @param logger  Initialized but stopped logger instance.
 * @retval 0 on success, negative errno on failure.
 */
int data_logger_start(struct data_logger *logger);

/**
 * @brief Return the human-readable name for an @ref aurora_data value.
 *
 * @param type  Sensor group identifier.
 * @return Null-terminated ASCII string, or @c "unknown" for out-of-range values.
 */
const char *data_logger_type_name(enum aurora_data type);

/* -------------------------------------------------------------------------- */
/*  Logger registry                                                           */
/* -------------------------------------------------------------------------- */

/**
 * @brief Callback type for @ref data_logger_foreach.
 *
 * @param logger  Registered logger instance.
 * @param user_data  Opaque context passed through from the caller.
 */
typedef void (*data_logger_cb_t)(struct data_logger *logger, void *user_data);

/**
 * @brief Iterate over all registered data loggers.
 *
 * @param cb         Callback invoked for each registered logger.
 * @param user_data  Opaque pointer forwarded to @p cb.
 */
void data_logger_foreach(data_logger_cb_t cb, void *user_data);

/**
 * @brief Look up a registered data logger by name.
 *
 * @param name  Logger name (as passed to @ref data_logger_init).
 * @return Pointer to the logger, or NULL if not found.
 */
struct data_logger *data_logger_get(const char *name);

/* -------------------------------------------------------------------------- */
/*  Built-in formatters                                                       */
/* -------------------------------------------------------------------------- */

#if defined(CONFIG_DATA_LOGGER_CSV)
/** CSV formatter — one header row then one row per datapoint. */
extern const struct data_logger_formatter data_logger_csv_formatter;
#endif

#if defined(CONFIG_DATA_LOGGER_INFLUX)
/** InfluxDB Line Protocol formatter — one line per datapoint, no header. */
extern const struct data_logger_formatter data_logger_influx_formatter;
#endif

#if defined(CONFIG_DATA_LOGGER_MOCK)
/** Mock formatter — provided by the test application. */
extern const struct data_logger_formatter data_logger_mock_formatter;
#endif

/** @} */

#endif /* AURORA_LIB_DATA_LOGGER_H_ */
