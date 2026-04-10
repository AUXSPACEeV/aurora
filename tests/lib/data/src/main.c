/**
 * @file main.c
 * @brief Unit tests for the data logger core and formatter backends.
 *
 * Three test suites are defined:
 *
 *  1. **data_logger_core** — tests the core dispatch layer and
 *     data_logger_type_name() using a lightweight mock formatter.
 *     No filesystem is required; this suite always runs.
 *
 *  2. **data_logger_csv** (CONFIG_DATA_LOGGER_CSV) — exercises the CSV
 *     formatter against a FatFS volume backed by a Zephyr RAM disk.  The
 *     volume is declared in the DTS overlay (zephyr,fstab,fatfs automount)
 *     and mounted by Zephyr's init system before any test runs.
 *
 *  3. **data_logger_influx** (CONFIG_DATA_LOGGER_INFLUX) — same as above
 *     for the InfluxDB Line Protocol formatter.
 *
 * Copyright (c) 2026 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <errno.h>

#include <zephyr/ztest.h>
#include <zephyr/fs/fs.h>

#include <aurora/lib/data_logger.h>

/* ========================================================================== */
/*  Filesystem helper (shared by CSV and InfluxDB suites)                     */
/*                                                                            */
/*  The FatFS volume on the RAM disk is declared with automount in the        */
/*  DTS overlay (zephyr,fstab,fatfs) and is mounted by Zephyr's init          */
/*  system before main() runs.  No explicit fs_mount() is needed here.        */
/* ========================================================================== */

#if defined(CONFIG_DATA_LOGGER_CSV) || defined(CONFIG_DATA_LOGGER_INFLUX)

/**
 * Read an entire file into @p buf (null-terminated).
 * @return number of bytes read, or negative errno.
 */
static int read_file(const char *path, char *buf, size_t size)
{
	struct fs_file_t f;

	fs_file_t_init(&f);

	int rc = fs_open(&f, path, FS_O_READ);

	if (rc < 0) {
		return rc;
	}

	ssize_t n = fs_read(&f, buf, size - 1);

	fs_close(&f);

	if (n < 0) {
		return (int)n;
	}

	buf[n] = '\0';
	return (int)n;
}

#endif /* CSV || INFLUX */

/* ========================================================================== */
/*  Mock formatter (used by core suite only)                                  */
/* ========================================================================== */

#if defined(CONFIG_DATA_LOGGER_MOCK)

static struct {
	int init_calls;
	int write_header_calls;
	int write_datapoint_calls;
	int flush_calls;
	int close_calls;
	int stop_calls;
	int start_calls;

	int fail_init;
	int fail_write_header;
	int fail_write_datapoint;
	int fail_flush;
	int fail_close;
	int fail_stop;
	int fail_start;

	struct datapoint last_dp;
	int ctx_token;
} mock_state;

static void mock_reset(void)
{
	memset(&mock_state, 0, sizeof(mock_state));
	mock_state.ctx_token = 0xAB;
}

static int mock_init(struct data_logger *logger, const char *path)
{
	(void)path;
	mock_state.init_calls++;
	if (mock_state.fail_init) {
		return -EIO;
	}
	logger->ctx = &mock_state.ctx_token;
	return 0;
}

static int mock_write_header(struct data_logger *logger)
{
	(void)logger;
	mock_state.write_header_calls++;
	return mock_state.fail_write_header ? -EIO : 0;
}

static int mock_write_datapoint(struct data_logger *logger,
				const struct datapoint *dp)
{
	(void)logger;
	mock_state.write_datapoint_calls++;
	mock_state.last_dp = *dp;
	return mock_state.fail_write_datapoint ? -EIO : 0;
}

static int mock_flush(struct data_logger *logger)
{
	(void)logger;
	mock_state.flush_calls++;
	return mock_state.fail_flush ? -EIO : 0;
}

static int mock_close(struct data_logger *logger)
{
	(void)logger;
	mock_state.close_calls++;
	return mock_state.fail_close ? -EIO : 0;
}

static int mock_stop(struct data_logger *logger)
{
	(void)logger;
	mock_state.stop_calls++;
	return mock_state.fail_stop ? -EIO : 0;
}

static int mock_start(struct data_logger *logger)
{
	(void)logger;
	mock_state.start_calls++;
	return mock_state.fail_start ? -EIO : 0;
}

const struct data_logger_formatter data_logger_mock_formatter = {
	.init            = mock_init,
	.write_header    = mock_write_header,
	.write_datapoint = mock_write_datapoint,
	.flush           = mock_flush,
	.close           = mock_close,
	.stop            = mock_stop,
	.start           = mock_start,
	.file_ext        = "mock",
	.name            = "mock",
};

#endif /* CONFIG_DATA_LOGGER_MOCK */

/* ========================================================================== */
/*  Suite 1: core dispatch tests (no filesystem)                              */
/* ========================================================================== */

#if defined(CONFIG_DATA_LOGGER_MOCK)

static struct data_logger logger;

static void core_before(void *fixture)
{
	(void)fixture;
	mock_reset();
	memset(&logger, 0, sizeof(logger));
}

ZTEST_SUITE(data_logger_core, NULL, NULL, core_before, NULL, NULL);

/* ---- data_logger_type_name ----------------------------------------------- */

ZTEST(data_logger_core, test_type_name_baro)
{
	zassert_str_equal(data_logger_type_name(AURORA_DATA_BARO), "baro",
			  "AURORA_DATA_BARO should map to \"baro\"");
}

ZTEST(data_logger_core, test_type_name_accel)
{
	zassert_str_equal(data_logger_type_name(AURORA_DATA_IMU_ACCEL),
			  "accel", NULL);
}

ZTEST(data_logger_core, test_type_name_imu_gyro)
{
	zassert_str_equal(data_logger_type_name(AURORA_DATA_IMU_GYRO),
			  "gyro", NULL);
}

ZTEST(data_logger_core, test_type_name_imu_mag)
{
	zassert_str_equal(data_logger_type_name(AURORA_DATA_IMU_MAG),
			  "mag", NULL);
}

ZTEST(data_logger_core, test_type_name_sentinel)
{
	zassert_str_equal(data_logger_type_name(AURORA_DATA_COUNT), "unknown",
			  "AURORA_DATA_COUNT should return \"unknown\"");
}

ZTEST(data_logger_core, test_type_name_out_of_range)
{
	zassert_str_equal(data_logger_type_name((enum aurora_data)255),
			  "unknown",
			  "Out-of-range type should return \"unknown\"");
}

/* ---- data_logger_init NULL guards ---------------------------------------- */

ZTEST(data_logger_core, test_init_null_logger)
{
	int rc = data_logger_init(NULL, "test");

	zassert_equal(rc, -EINVAL, NULL);
	zassert_equal(mock_state.init_calls, 0, NULL);
}

ZTEST(data_logger_core, test_init_null_filename)
{
	int rc = data_logger_init(&logger, NULL);

	zassert_equal(rc, -EINVAL, NULL);
	zassert_equal(mock_state.init_calls, 0, NULL);
}

/* ---- data_logger_init error propagation ---------------------------------- */

ZTEST(data_logger_core, test_init_formatter_init_fails)
{
	mock_state.fail_init = 1;

	int rc = data_logger_init(&logger, "test");

	zassert_equal(rc, -EIO, NULL);
	zassert_equal(mock_state.init_calls, 1, NULL);
	zassert_equal(mock_state.write_header_calls, 0, NULL);
}

ZTEST(data_logger_core, test_init_write_header_fails)
{
	mock_state.fail_write_header = 1;

	int rc = data_logger_init(&logger, "test");

	zassert_equal(rc, -EIO, NULL);
	zassert_equal(mock_state.write_header_calls, 1, NULL);
	zassert_equal(mock_state.close_calls, 1,
		      "close must be called to clean up after write_header "
		      "failure");
}

ZTEST(data_logger_core, test_init_success)
{
	int rc = data_logger_init(&logger, "test");

	zassert_ok(rc, NULL);
	zassert_equal(mock_state.init_calls, 1, NULL);
	zassert_equal(mock_state.write_header_calls, 1, NULL);
	zassert_not_null(logger.fmt, NULL);
	zassert_not_null(logger.ctx, NULL);

	zassert_ok(data_logger_close(&logger), NULL);
}

/* ---- data_logger_log ----------------------------------------------------- */

ZTEST(data_logger_core, test_log_null_logger)
{
	struct datapoint dp = {0};

	zassert_equal(data_logger_write(NULL, &dp), -EINVAL, NULL);
}

ZTEST(data_logger_core, test_log_null_dp)
{
	data_logger_init(&logger, "test");
	zassert_equal(data_logger_write(&logger, NULL), -EINVAL, NULL);
	zassert_equal(mock_state.write_datapoint_calls, 0, NULL);
	zassert_ok(data_logger_close(&logger), NULL);
}

ZTEST(data_logger_core, test_log_after_close)
{
	data_logger_init(&logger, "test");
	data_logger_close(&logger);
	mock_reset();

	struct datapoint dp = {0};

	zassert_equal(data_logger_write(&logger, &dp), -EINVAL, NULL);
}

ZTEST(data_logger_core, test_log_delegates_datapoint)
{
	data_logger_init(&logger, "test");

	struct datapoint dp = {
		.timestamp_ms  = 12345LL,
		.type          = AURORA_DATA_IMU_ACCEL,
		.channel_count = 3,
		.channels = {
			{.val1 = 1, .val2 = 0},
			{.val1 = 2, .val2 = 0},
			{.val1 = 3, .val2 = 0},
		},
	};

	zassert_ok(data_logger_write(&logger, &dp), NULL);
	zassert_equal(mock_state.write_datapoint_calls, 1, NULL);
	zassert_equal(mock_state.last_dp.timestamp_ms, 12345LL,
		      "timestamp must be forwarded unchanged");
	zassert_equal(mock_state.last_dp.type, AURORA_DATA_IMU_ACCEL, NULL);
	zassert_equal(mock_state.last_dp.channel_count, 3, NULL);

	zassert_ok(data_logger_close(&logger), NULL);
}

ZTEST(data_logger_core, test_log_error_propagated)
{
	data_logger_init(&logger, "test");
	mock_state.fail_write_datapoint = 1;

	struct datapoint dp = {.type = AURORA_DATA_BARO, .channel_count = 2};

	zassert_equal(data_logger_write(&logger, &dp), -EIO, NULL);
	zassert_ok(data_logger_close(&logger), NULL);
}

/* ---- data_logger_flush --------------------------------------------------- */

ZTEST(data_logger_core, test_flush_null_logger)
{
	zassert_equal(data_logger_flush(NULL), -EINVAL, NULL);
}

ZTEST(data_logger_core, test_flush_delegates)
{
	data_logger_init(&logger, "test");
	int before = mock_state.flush_calls;
	zassert_ok(data_logger_flush(&logger), NULL);
	zassert_equal(mock_state.flush_calls, before + 1, NULL);

	zassert_ok(data_logger_close(&logger), NULL);
}

ZTEST(data_logger_core, test_flush_error_propagated)
{
	data_logger_init(&logger, "test");
	mock_state.fail_flush = 1;
	zassert_equal(data_logger_flush(&logger), -EIO, NULL);

	zassert_ok(data_logger_close(&logger), NULL);
}

/* ---- data_logger_close --------------------------------------------------- */

ZTEST(data_logger_core, test_close_null_logger)
{
	zassert_equal(data_logger_close(NULL), -EINVAL, NULL);
}

ZTEST(data_logger_core, test_close_resets_fields)
{
	data_logger_init(&logger, "test");
	zassert_ok(data_logger_close(&logger), NULL);
	zassert_equal(mock_state.close_calls, 1, NULL);
	zassert_is_null(logger.fmt, "logger.fmt must be NULL after close");
	zassert_is_null(logger.ctx, "logger.ctx must be NULL after close");
}

ZTEST(data_logger_core, test_close_error_propagated)
{
	data_logger_init(&logger, "test");
	mock_state.fail_close = 1;
	zassert_equal(data_logger_close(&logger), -EIO, NULL);
	/* Fields must be reset even on error. */
	zassert_is_null(logger.fmt, NULL);
	zassert_is_null(logger.state, NULL);
	zassert_is_null(logger.ctx, NULL);
}

/* ---- data_logger_stop --------------------------------------------------- */

ZTEST(data_logger_core, test_stop_null_logger)
{
	zassert_equal(data_logger_stop(NULL), -EINVAL, NULL);
}

ZTEST(data_logger_core, test_stop_delegates)
{
	data_logger_init(&logger, "test");
	int before = mock_state.stop_calls;
	zassert_ok(data_logger_stop(&logger), NULL);
	zassert_equal(mock_state.stop_calls, before + 1, NULL);

	/* start again */
	zassert_ok(data_logger_start(&logger), NULL);

	zassert_ok(data_logger_close(&logger), NULL);
}

ZTEST(data_logger_core, test_stop_error_propagated)
{
	data_logger_init(&logger, "test");
	mock_state.fail_stop = 1;
	zassert_equal(data_logger_stop(&logger), -EIO, NULL);

	zassert_ok(data_logger_close(&logger), NULL);
}

/* ---- data_logger_start --------------------------------------------------- */

ZTEST(data_logger_core, test_start_null_logger)
{
	zassert_equal(data_logger_start(NULL), -EINVAL, NULL);
}

ZTEST(data_logger_core, test_start_delegates)
{
	data_logger_init(&logger, "test");
	int before = mock_state.start_calls;
	zassert_ok(data_logger_start(&logger), NULL);
	zassert_equal(mock_state.start_calls, before + 1, NULL);

	zassert_ok(data_logger_close(&logger), NULL);
}

ZTEST(data_logger_core, test_start_error_propagated)
{
	data_logger_init(&logger, "test");
	mock_state.fail_start = 1;
	zassert_equal(data_logger_start(&logger), -EIO, NULL);

	zassert_ok(data_logger_close(&logger), NULL);
}

/* ---- Registry: data_logger_get ------------------------------------------ */

ZTEST(data_logger_core, test_registry_get_after_init)
{
	zassert_ok(data_logger_init(&logger, "test"), NULL);

	struct data_logger *found = data_logger_get("test");

	zassert_equal(found, &logger,
		      "data_logger_get must return the registered logger");

	zassert_ok(data_logger_close(&logger), NULL);
}

ZTEST(data_logger_core, test_registry_get_not_found)
{
	struct data_logger *found = data_logger_get("nonexistent");

	zassert_is_null(found,
			"data_logger_get must return NULL for unknown name");
}

ZTEST(data_logger_core, test_registry_get_removed_after_close)
{
	zassert_ok(data_logger_init(&logger, "gone"), NULL);
	zassert_ok(data_logger_close(&logger), NULL);

	struct data_logger *found = data_logger_get("gone");

	zassert_is_null(found,
			"Closed logger must be removed from the registry");
}

/* ---- Registry: data_logger_foreach -------------------------------------- */

static int foreach_count;
static struct data_logger *foreach_last;

static void counting_cb(struct data_logger *l, void *user_data)
{
	foreach_count++;
	foreach_last = l;
	int *acc = user_data;
	if (acc != NULL) {
		(*acc)++;
	}
}

ZTEST(data_logger_core, test_registry_foreach_empty)
{
	foreach_count = 0;
	foreach_last = NULL;

	data_logger_foreach(counting_cb, NULL);

	zassert_equal(foreach_count, 0,
		      "foreach must not invoke callback when registry is empty");
}

ZTEST(data_logger_core, test_registry_foreach_visits_logger)
{
	zassert_ok(data_logger_init(&logger, "iter"), NULL);

	foreach_count = 0;
	foreach_last = NULL;
	int user_acc = 0;

	data_logger_foreach(counting_cb, &user_acc);

	zassert_equal(foreach_count, 1, NULL);
	zassert_equal(foreach_last, &logger, NULL);
	zassert_equal(user_acc, 1,
		      "user_data must be forwarded to the callback");

	zassert_ok(data_logger_close(&logger), NULL);
}

ZTEST(data_logger_core, test_registry_foreach_multiple)
{
	static struct data_logger loggers[3];

	memset(loggers, 0, sizeof(loggers));

	zassert_ok(data_logger_init(&loggers[0], "a"), NULL);
	zassert_ok(data_logger_init(&loggers[1], "b"), NULL);
	zassert_ok(data_logger_init(&loggers[2], "c"), NULL);

	foreach_count = 0;
	data_logger_foreach(counting_cb, NULL);

	zassert_equal(foreach_count, 3,
		      "foreach must visit all registered loggers");

	zassert_ok(data_logger_close(&loggers[0]), NULL);
	zassert_ok(data_logger_close(&loggers[1]), NULL);
	zassert_ok(data_logger_close(&loggers[2]), NULL);
}

/* ---- Registry: capacity ------------------------------------------------- */

ZTEST(data_logger_core, test_registry_full)
{
	static struct data_logger loggers[CONFIG_DATA_LOGGER_MAX_LOGGERS + 1];
	int i;

	memset(loggers, 0, sizeof(loggers));

	/* Fill the registry to capacity. */
	for (i = 0; i < CONFIG_DATA_LOGGER_MAX_LOGGERS; i++) {
		char name[DATA_LOGGER_NAME_MAX];

		snprintf(name, sizeof(name), "lg%d", i);
		zassert_ok(data_logger_init(&loggers[i], name),
			   "Init of logger %d must succeed", i);
	}

	/* One more must fail with -ENOMEM. */
	int rc = data_logger_init(&loggers[i], "overflow");

	zassert_equal(rc, -ENOMEM,
		      "Exceeding MAX_LOGGERS must return -ENOMEM");

	/* Clean up. */
	for (i = 0; i < CONFIG_DATA_LOGGER_MAX_LOGGERS; i++) {
		zassert_ok(data_logger_close(&loggers[i]), NULL);
	}
}

/* ---- Full lifecycle (mock) ----------------------------------------------- */

ZTEST(data_logger_core, test_full_lifecycle)
{
	zassert_ok(data_logger_init(&logger, "test"), NULL);

	static const struct {
		enum aurora_data type;
		uint8_t channel_count;
	} cases[] = {
		{AURORA_DATA_BARO,      2},
		{AURORA_DATA_IMU_ACCEL, 3},
		{AURORA_DATA_IMU_GYRO,  3},
		{AURORA_DATA_IMU_MAG,   3},
	};

	for (int i = 0; i < (int)ARRAY_SIZE(cases); i++) {
		struct datapoint dp = {
			.timestamp_ms  = (int64_t)(i + 1) * 10,
			.type          = cases[i].type,
			.channel_count = cases[i].channel_count,
		};
		zassert_ok(data_logger_write(&logger, &dp), NULL);
	}

	zassert_equal(mock_state.write_datapoint_calls, (int)ARRAY_SIZE(cases),
		      NULL);
	zassert_ok(data_logger_flush(&logger), NULL);
	zassert_ok(data_logger_close(&logger), NULL);
	zassert_equal(mock_state.init_calls,         1, NULL);
	zassert_equal(mock_state.write_header_calls, 1, NULL);
	zassert_equal(mock_state.flush_calls,        2, NULL);
	zassert_equal(mock_state.close_calls,        1, NULL);
}

#endif /* CONFIG_DATA_LOGGER_MOCK */

/* ========================================================================== */
/*  Suite 2: CSV formatter                                                    */
/* ========================================================================== */

#if defined(CONFIG_DATA_LOGGER_CSV)

#define CSV_FILE_PATH CONFIG_DATA_LOGGER_BASE_PATH "/test_0.csv"
#define CSV_BUF_SIZE 512

static struct data_logger csv_logger;

static void csv_before(void *fixture)
{
	(void)fixture;
	memset(&csv_logger, 0, sizeof(csv_logger));
	/* Remove any leftover file from a previous test. */
	fs_unlink(CSV_FILE_PATH);
}

ZTEST_SUITE(data_logger_csv, NULL, NULL, csv_before, NULL, NULL);

/**
 * @brief CSV header row is written on init.
 *
 * The first line of the file must contain "timestamp_ms" and "type".
 */
ZTEST(data_logger_csv, test_csv_header_present)
{
	char buf[CSV_BUF_SIZE];

	zassert_ok(data_logger_init(&csv_logger, "test"), NULL);
	zassert_ok(data_logger_close(&csv_logger), NULL);

	int n = read_file(CSV_FILE_PATH, buf, sizeof(buf));

	zassert_true(n > 0, "File should not be empty after init+close");
	zassert_not_null(strstr(buf, "timestamp_ms"),
			 "Header must contain \"timestamp_ms\"");
	zassert_not_null(strstr(buf, "type"),
			 "Header must contain \"type\"");
}

/**
 * @brief Barometer datapoint is written with correct timestamp and type name.
 */
ZTEST(data_logger_csv, test_csv_baro_datapoint)
{
	char buf[CSV_BUF_SIZE];

	struct datapoint dp = {
		.timestamp_ms  = 1000LL,
		.type          = AURORA_DATA_BARO,
		.channel_count = 2,
		.channels = {
			{.val1 = 23, .val2 = 500000},  /* 23.5 °C  */
			{.val1 = 101325, .val2 = 0},   /* 101325 Pa */
		},
	};

	zassert_ok(data_logger_init(&csv_logger, "test"), NULL);
	zassert_ok(data_logger_write(&csv_logger, &dp), NULL);
	zassert_ok(data_logger_close(&csv_logger), NULL);

	int n = read_file(CSV_FILE_PATH, buf, sizeof(buf));

	zassert_true(n > 0, "File must not be empty");
	zassert_not_null(strstr(buf, "1000"),
			 "Timestamp 1000 must appear in file");
	zassert_not_null(strstr(buf, "baro"),
			 "Type name \"baro\" must appear in data row");
	zassert_not_null(strstr(buf, "23.500000"),
			 "Temperature value 23.500000 must appear");
	zassert_not_null(strstr(buf, "101325.000000"),
			 "Pressure value 101325.000000 must appear");
}

/**
 * @brief IMU accelerometer datapoint (3 channels) is written correctly.
 */
ZTEST(data_logger_csv, test_csv_accel_datapoint)
{
	char buf[CSV_BUF_SIZE];

	struct datapoint dp = {
		.timestamp_ms  = 2000LL,
		.type          = AURORA_DATA_IMU_ACCEL,
		.channel_count = 3,
		.channels = {
			{.val1 = 0, .val2 = 100000},  /* 0.1 m/s² x */
			{.val1 = 0, .val2 = 200000},  /* 0.2 m/s² y */
			{.val1 = 9, .val2 = 810000},  /* 9.81 m/s² z */
		},
	};

	zassert_ok(data_logger_init(&csv_logger, "test"), NULL);
	zassert_ok(data_logger_write(&csv_logger, &dp), NULL);
	zassert_ok(data_logger_close(&csv_logger), NULL);

	int n = read_file(CSV_FILE_PATH, buf, sizeof(buf));

	zassert_true(n > 0, NULL);
	zassert_not_null(strstr(buf, "accel"), NULL);
	zassert_not_null(strstr(buf, "0.100000"), NULL);
	zassert_not_null(strstr(buf, "0.200000"), NULL);
	zassert_not_null(strstr(buf, "9.810000"), NULL);
}

/**
 * @brief Negative channel value is serialised with a leading minus sign.
 */
ZTEST(data_logger_csv, test_csv_negative_value)
{
	char buf[CSV_BUF_SIZE];

	struct datapoint dp = {
		.timestamp_ms  = 500LL,
		.type          = AURORA_DATA_IMU_GYRO,
		.channel_count = 1,
		.channels = {
			{.val1 = -5, .val2 = -250000},  /* -5.25 °/s */
		},
	};

	zassert_ok(data_logger_init(&csv_logger, "test"), NULL);
	zassert_ok(data_logger_write(&csv_logger, &dp), NULL);
	zassert_ok(data_logger_close(&csv_logger), NULL);

	int n = read_file(CSV_FILE_PATH, buf, sizeof(buf));

	zassert_true(n > 0, NULL);
	zassert_not_null(strstr(buf, "-5.250000"),
			 "Negative value must be prefixed with '-'");
}

/**
 * @brief Multiple datapoints produce multiple data rows.
 */
ZTEST(data_logger_csv, test_csv_multiple_rows)
{
	char buf[CSV_BUF_SIZE];

	zassert_ok(data_logger_init(&csv_logger, "test"), NULL);

	for (int i = 0; i < 3; i++) {
		struct datapoint dp = {
			.timestamp_ms  = (int64_t)(i + 1) * 100,
			.type          = AURORA_DATA_BARO,
			.channel_count = 2,
			.channels = {
				{.val1 = 20 + i, .val2 = 0},
				{.val1 = 101000 + i * 100, .val2 = 0},
			},
		};
		zassert_ok(data_logger_write(&csv_logger, &dp), NULL);
	}

	zassert_ok(data_logger_close(&csv_logger), NULL);

	int n = read_file(CSV_FILE_PATH, buf, sizeof(buf));

	zassert_true(n > 0, NULL);

	/* Count newlines: 1 header + 3 data rows = 4 lines. */
	int newlines = 0;

	for (int i = 0; i < n; i++) {
		if (buf[i] == '\n') {
			newlines++;
		}
	}

	zassert_equal(newlines, 4,
		      "Expected 1 header row + 3 data rows = 4 newlines");
}

#endif /* CONFIG_DATA_LOGGER_CSV */

/* ================================================================== */
/*  Suite 3: InfluxDB Line Protocol formatter                          */
/* ================================================================== */

#if defined(CONFIG_DATA_LOGGER_INFLUX)

#define INFLUX_FILE_PATH CONFIG_DATA_LOGGER_BASE_PATH "/test_0.influx"
#define INFLUX_BUF_SIZE 512

static struct data_logger influx_logger;

static void influx_before(void *fixture)
{
	(void)fixture;
	memset(&influx_logger, 0, sizeof(influx_logger));
	fs_unlink(INFLUX_FILE_PATH);
}

ZTEST_SUITE(data_logger_influx, NULL, NULL, influx_before, NULL, NULL);

/**
 * @brief InfluxDB init produces no header row (file stays empty on close).
 */
ZTEST(data_logger_influx, test_influx_no_header)
{
	char buf[INFLUX_BUF_SIZE];

	zassert_ok(data_logger_init(&influx_logger, "test"), NULL);
	zassert_ok(data_logger_close(&influx_logger), NULL);

	int n = read_file(INFLUX_FILE_PATH, buf, sizeof(buf));

	zassert_equal(n, 0, "InfluxDB formatter must not write a header row");
}

/**
 * @brief Barometer datapoint produces correct InfluxDB line.
 *
 * Expected format:
 *   telemetry,type=baro temp=23.500000,pres=101325.000000 1000\n
 */
ZTEST(data_logger_influx, test_influx_baro_line)
{
	char buf[INFLUX_BUF_SIZE];

	struct datapoint dp = {
		.timestamp_ms  = 1000LL,
		.type          = AURORA_DATA_BARO,
		.channel_count = 2,
		.channels = {
			{.val1 = 23, .val2 = 500000},  /* 23.5 °C  */
			{.val1 = 101325, .val2 = 0},   /* 101325 Pa */
		},
	};

	zassert_ok(data_logger_init(&influx_logger, "test"), NULL);
	zassert_ok(data_logger_write(&influx_logger, &dp), NULL);
	zassert_ok(data_logger_close(&influx_logger), NULL);

	int n = read_file(INFLUX_FILE_PATH, buf, sizeof(buf));

	zassert_true(n > 0, "File must not be empty after logging");

	/* Measurement tag and type */
	zassert_not_null(strstr(buf, "telemetry,type=baro"),
			 "Line must start with \"telemetry,type=baro\"");

	/* Field names */
	zassert_not_null(strstr(buf, "temp="),
			 "Baro line must contain temp field");
	zassert_not_null(strstr(buf, "pres="),
			 "Baro line must contain pres field");

	/* Field values */
	zassert_not_null(strstr(buf, "23.500000"), NULL);
	zassert_not_null(strstr(buf, "101325.000000"), NULL);

	/* Timestamp at end of line */
	zassert_not_null(strstr(buf, "1000"),
			 "Timestamp 1000 must be present");
}

/**
 * @brief IMU gyroscope datapoint uses x/y/z field names.
 */
ZTEST(data_logger_influx, test_influx_imu_gyro_fields)
{
	char buf[INFLUX_BUF_SIZE];

	struct datapoint dp = {
		.timestamp_ms  = 2500LL,
		.type          = AURORA_DATA_IMU_GYRO,
		.channel_count = 3,
		.channels = {
			{.val1 = 1, .val2 = 100000},
			{.val1 = 2, .val2 = 200000},
			{.val1 = 3, .val2 = 300000},
		},
	};

	zassert_ok(data_logger_init(&influx_logger, "test"), NULL);
	zassert_ok(data_logger_write(&influx_logger, &dp), NULL);
	zassert_ok(data_logger_close(&influx_logger), NULL);

	read_file(INFLUX_FILE_PATH, buf, sizeof(buf));

	zassert_not_null(strstr(buf, "type=gyro"), NULL);
	zassert_not_null(strstr(buf, "x="), "IMU line must contain x field");
	zassert_not_null(strstr(buf, "y="), "IMU line must contain y field");
	zassert_not_null(strstr(buf, "z="), "IMU line must contain z field");
	zassert_not_null(strstr(buf, "1.100000"), NULL);
	zassert_not_null(strstr(buf, "2.200000"), NULL);
	zassert_not_null(strstr(buf, "3.300000"), NULL);
}

/**
 * @brief Negative channel value is serialised with a leading minus sign.
 */
ZTEST(data_logger_influx, test_influx_negative_value)
{
	char buf[INFLUX_BUF_SIZE];

	struct datapoint dp = {
		.timestamp_ms  = 300LL,
		.type          = AURORA_DATA_IMU_ACCEL,
		.channel_count = 1,
		.channels = {
			{.val1 = -9, .val2 = -810000},  /* -9.81 m/s² */
		},
	};

	zassert_ok(data_logger_init(&influx_logger, "test"), NULL);
	zassert_ok(data_logger_write(&influx_logger, &dp), NULL);
	zassert_ok(data_logger_close(&influx_logger), NULL);

	read_file(INFLUX_FILE_PATH, buf, sizeof(buf));

	zassert_not_null(strstr(buf, "-9.810000"),
			 "Negative value must appear with leading '-'");
}

/**
 * @brief Multiple datapoints produce one line each (no blank lines).
 */
ZTEST(data_logger_influx, test_influx_multiple_lines)
{
	char buf[INFLUX_BUF_SIZE];

	zassert_ok(data_logger_init(&influx_logger, "test"), NULL);

	static const enum aurora_data types[] = {
		AURORA_DATA_BARO,
		AURORA_DATA_IMU_ACCEL,
		AURORA_DATA_IMU_GYRO,
		AURORA_DATA_IMU_MAG,
	};

	for (int i = 0; i < (int)ARRAY_SIZE(types); i++) {
		uint8_t ch = (types[i] == AURORA_DATA_BARO) ? 2 : 3;
		struct datapoint dp = {
			.timestamp_ms  = (int64_t)(i + 1) * 10,
			.type          = types[i],
			.channel_count = ch,
		};
		zassert_ok(data_logger_write(&influx_logger, &dp), NULL);
	}

	zassert_ok(data_logger_close(&influx_logger), NULL);

	int n = read_file(INFLUX_FILE_PATH, buf, sizeof(buf));

	zassert_true(n > 0, NULL);

	/* Exactly 4 newlines: one per datapoint, no header. */
	int newlines = 0;

	for (int i = 0; i < n; i++) {
		if (buf[i] == '\n') {
			newlines++;
		}
	}

	zassert_equal(newlines, 4,
		      "Expected exactly 4 lines (one per datapoint)");
}

#endif /* CONFIG_DATA_LOGGER_INFLUX */
