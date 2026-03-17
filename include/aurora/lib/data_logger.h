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
 *   data_logger_init(&logger, &data_logger_csv_formatter, "/lfs/log.csv");
 *   data_logger_log(&logger, &dp);
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
 * Implement all five callbacks and export a
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
};

/**
 * @brief Logger instance (caller-allocated, typically stack or static).
 */
struct data_logger {
	const struct data_logger_formatter *fmt; /**< Active formatter vtable  */
	void *ctx;                               /**< Opaque formatter context */
};

/**
 * @brief Initialise a logger with the given formatter and output path.
 *
 * Calls @c fmt->init then @c fmt->write_header.  On any failure the logger
 * is left in an invalid state and should not be used.
 *
 * @param logger  Caller-allocated logger instance.
 * @param fmt     Formatter vtable to use.
 * @param path    Filesystem path for the output file.
 * @retval 0 on success, negative errno on failure.
 */
int data_logger_init(struct data_logger *logger,
		     const struct data_logger_formatter *fmt,
		     const char *path);

/**
 * @brief Serialise and store one datapoint.
 *
 * @param logger  Initialised logger instance.
 * @param dp      Datapoint to write.
 * @retval 0 on success, negative errno on failure.
 */
int data_logger_log(struct data_logger *logger, const struct datapoint *dp);

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
 * @brief Return the human-readable name for an @ref aurora_data value.
 *
 * @param type  Sensor group identifier.
 * @return Null-terminated ASCII string, or @c "unknown" for out-of-range values.
 */
const char *data_logger_type_name(enum aurora_data type);

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

/** @} */

#endif /* AURORA_LIB_DATA_LOGGER_H_ */
