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
	AURORA_DATA_BARO,          /**< Barometer: [0] temperature, [1] pressure  */
	AURORA_DATA_IMU_ACCEL,     /**< Accelerometer: [0] x, [1] y, [2] z        */
	AURORA_DATA_IMU_GYRO,      /**< Gyroscope:     [0] x, [1] y, [2] z        */
	AURORA_DATA_IMU_MAG,       /**< Magnetometer:  [0] x, [1] y, [2]  z       */
	AURORA_DATA_SM_KINEMATICS, /**< SM inputs: [0] accel, [1] accel_vert      */
	AURORA_DATA_SM_POSE,       /**< SM Pose: [0] velocity, [1] altitude       */
	AURORA_DATA_ORIENTATION,   /**< Orientation: [0] yaw, [1] pitch, [2] roll */
	AURORA_DATA_COUNT,         /**< Sentinel — do not use as a type           */
};

/**
 * @brief Flat telemetry data point.
 *
 * Carries a timestamp, sensor-group type, the number of valid channels, and
 * up to @ref DP_MAX_CHANNELS raw Zephyr @c sensor_value readings.
 */
struct datapoint {
	uint64_t timestamp_ns;                         /**< ns since launch           */
	enum aurora_data type;                         /**< Sensor group              */
	uint8_t channel_count;                         /**< Valid entries in channels */
	struct sensor_value channels[DP_MAX_CHANNELS]; /**< Channel readings          */
};

/** Forward declaration (needed by formatter callbacks). */
struct data_logger;

/**
 * @brief Lifecycle events the upstream application can deliver to a
 *        formatter via @ref data_logger_event.
 *
 * The @ref lib_data_logger_bin formatter uses these to switch from
 * circular pre-boost capture to linear "freeze" mode at BOOST and to
 * mark the end of the recorded flight at LANDED.  Other formatters may
 * ignore them.
 */
enum data_logger_event {
	DLE_BOOST,	/**< Boost detected; freeze the ring forward from here. */
	DLE_LANDED,	/**< Landed; the post-landed pad window starts now.     */
};

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

	/**
	 * Optional flight-lifecycle event hook (BOOST / LANDED).
	 *
	 * Called from @ref data_logger_event under the logger mutex.
	 * Formatters that don't care about transitions may leave this NULL.
	 */
	int (*on_event)(struct data_logger *logger, enum data_logger_event ev);

	/** File suffix */
	char file_ext[8];

	/** Human-readable formatter name (e.g. "csv", "influx", "mock") */
	char name[8];
};

/**
 * @brief Formatter state.
 */
struct data_logger_state {
	/** Mutex for thread safety around I/O operations */
	struct k_mutex mutex;

	/** Data logger is running and logging (atomic for ISR access) */
	atomic_t running;
};

/** Maximum length of a data logger name (including NUL). */
#define DATA_LOGGER_NAME_MAX 16

/** Maximum length of a data logger path (including NUL). */
#define DATA_LOGGER_PATH_MAX 64

/**
 * @brief Logger instance (caller-allocated, typically stack or static).
 */
struct data_logger {
	const struct data_logger_formatter *fmt; /**< Active formatter vtable  */
	struct data_logger_state *state;         /**< Active formatter state */
	void *ctx;                               /**< Opaque formatter context */
	char name[DATA_LOGGER_NAME_MAX];        /**< Logger name (set by init) */
	char path[DATA_LOGGER_PATH_MAX];        /**< Full output path (set by init) */
};

/**
 * @brief Initialise a logger.
 *
 * The output file is placed at
 * @c CONFIG_DATA_LOGGER_BASE_PATH/[ @p filename ]_N.file_ext, where N is
 * a free rotation index and file_ext comes from @p fmt.  Calls
 * @c fmt->init then @c fmt->write_header.  On any failure the logger is
 * left in an invalid state and should not be used.
 *
 * @param logger    Caller-allocated logger instance.
 * @param filename  Base filename (without extension).
 * @param fmt       Formatter vtable (e.g. @c data_logger_bin_formatter).
 * @retval 0 on success, negative errno on failure.
 */
int data_logger_init(struct data_logger *logger, const char *filename,
		     const struct data_logger_formatter *fmt);

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
 * @brief Deliver a flight-lifecycle event to the formatter.
 *
 * Dispatches to the formatter's optional @c on_event hook under the
 * logger mutex.  Formatters that do not implement the hook silently
 * accept the event.
 *
 * @param logger  Initialised logger instance.
 * @param ev      Event to deliver.
 * @retval 0 on success or no-op, negative errno on failure.
 */
int data_logger_event(struct data_logger *logger, enum data_logger_event ev);

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

#if defined(CONFIG_DATA_LOGGER_BIN)
/** Fixed-size binary formatter — live flight-time log (see @ref data_logger_bin). */
extern const struct data_logger_formatter data_logger_bin_formatter;
#endif

#if defined(CONFIG_DATA_LOGGER_CONVERT_CSV)
/** CSV formatter — conversion target. One header row, one row per datapoint. */
extern const struct data_logger_formatter data_logger_csv_formatter;
#endif

#if defined(CONFIG_DATA_LOGGER_CONVERT_INFLUX)
/** InfluxDB Line Protocol formatter — conversion target. */
extern const struct data_logger_formatter data_logger_influx_formatter;
#endif

#if defined(CONFIG_DATA_LOGGER_MOCK)
/** Mock formatter — provided by the test application. */
extern const struct data_logger_formatter data_logger_mock_formatter;
#endif

/* -------------------------------------------------------------------------- */
/*  Binary record layout (live flight-time log)                               */
/* -------------------------------------------------------------------------- */

/**
 * @defgroup lib_data_logger_bin Binary log format
 * @ingroup lib_data_logger
 * @{
 *
 * The flight-time log is written to a raw flash partition (no filesystem).
 * The partition is laid out as a sequence of fixed-size @b frames, each
 * sized to one flash erase block (@c CONFIG_DATA_LOGGER_BIN_FRAME_SIZE,
 * typically 4096 B).  Each frame begins with an @ref aurora_bin_frame_header
 * followed by densely packed @ref aurora_bin_record entries; any unused
 * bytes at the tail of a partially-filled frame remain in the erased
 * (0xFF) state.
 *
 * Records preserve the @c sensor_value channels @b losslessly so the host
 * can re-run filters and the state machine bit-exactly.  Per-record
 * timestamps are stored as a @c uint32_t µs delta from the frame's
 * @c base_ts_ns; nanosecond precision below 1 µs is dropped (the upstream
 * tick clock is already coarser than 1 µs on every supported target).
 *
 * All multi-byte integers are native little-endian.
 */

/** 4-byte magic string at the start of every frame. */
#define AURORA_BIN_FRAME_MAGIC "AURF"

/** Current binary format version (bumped from 1 → 2 for the frame layout). */
#define AURORA_BIN_VERSION 2U

/** Per-frame header (32 bytes). */
struct aurora_bin_frame_header {
	char     magic[4];        /**< @ref AURORA_BIN_FRAME_MAGIC */
	uint16_t version;         /**< @ref AURORA_BIN_VERSION */
	uint16_t reserved0;       /**< Zero */
	uint32_t seq;             /**< Monotonic, starts at 0 each flight */
	uint64_t flight_id;       /**< Unique per flight (uptime at start) */
	uint64_t base_ts_ns;      /**< Absolute reference for record deltas */
	uint32_t reserved1;       /**< Zero (pad to 32 B / record alignment) */
} __packed;

/** Per-datapoint record (32 bytes). Channels are stored losslessly. */
struct aurora_bin_record {
	uint8_t  type;            /**< @ref aurora_data; 0xFF marks end-of-frame */
	uint8_t  channel_count;   /**< Valid channels in @c channels */
	uint16_t reserved;        /**< Zero */
	uint32_t ts_delta_us;     /**< µs since this frame's base_ts_ns */
	struct {
		int32_t val1;     /**< sensor_value.val1 (integer part) */
		int32_t val2;     /**< sensor_value.val2 (µ-fractional part) */
	} channels[DP_MAX_CHANNELS];
} __packed;

BUILD_ASSERT(sizeof(struct aurora_bin_frame_header) == 32,
	     "frame header size changed — bump AURORA_BIN_VERSION");
BUILD_ASSERT(sizeof(struct aurora_bin_record) == 32,
	     "record size changed — bump AURORA_BIN_VERSION");

/**
 * @brief Convert the live flight log on raw flash to a text formatter output.
 *
 * Walks frames in the configured @c auxspace,flight-log partition starting
 * at offset 0; for each valid frame reconstructs the original
 * @ref datapoint stream and feeds it through @p out_fmt at @p out_path.
 * Reading stops at the first frame whose magic is invalid (== unwritten
 * flash) or whose @c flight_id differs from the first frame's — that's
 * how the boundary between the current flight and any leftover data from
 * a previous flight is detected.
 *
 * The flash partition is left intact.  The caller is responsible for not
 * running conversion concurrently with active logging.
 *
 * @param out_fmt   Target formatter (e.g. @c data_logger_csv_formatter).
 * @param out_path  Destination path for the converted output.
 * @retval 0 on success, negative errno on failure.
 */
int data_logger_convert(const struct data_logger_formatter *out_fmt,
			const char *out_path);

/** @} */

/** @} */

#endif /* AURORA_LIB_DATA_LOGGER_H_ */
