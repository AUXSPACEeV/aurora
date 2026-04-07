/**
 * @file data_logger.c
 * @brief Core data-logger logic and sensor-group name table.
 *
 * Implements data_logger_init / data_logger_log / data_logger_flush /
 * data_logger_close by dispatching through the formatter vtable, and
 * provides data_logger_type_name() for formatter backends.
 *
 * Copyright (c) 2026 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>

#include <aurora/lib/data_logger.h>

LOG_MODULE_REGISTER(data_logger, CONFIG_DATA_LOGGER_LOG_LEVEL);

/* -------------------------------------------------------------------------- */
/*  Sensor-group name table                                                   */
/* -------------------------------------------------------------------------- */

static const char *const aurora_data_names[AURORA_DATA_COUNT] = {
	[AURORA_DATA_BARO]      = "baro",
	[AURORA_DATA_IMU_ACCEL] = "imu_accel",
	[AURORA_DATA_IMU_GYRO]  = "imu_gyro",
	[AURORA_DATA_IMU_MAG]   = "imu_mag",
};

/* data_logger_type_name – see data_logger.h */
const char *data_logger_type_name(enum aurora_data type)
{
	if (type >= AURORA_DATA_COUNT)
		return "unknown";

	return aurora_data_names[type];
}

/* -------------------------------------------------------------------------- */
/*  Public API                                                                */
/* -------------------------------------------------------------------------- */

/* data_logger_init – see data_logger.h */
int data_logger_init(struct data_logger *logger,
		     const struct data_logger_formatter *fmt,
		     const char *path)
{
	struct fs_dir_t ptr;
	char dir[64];
	char *sep;
	int rc;

	if (logger == NULL || fmt == NULL || path == NULL)
		return -EINVAL;

	logger->fmt = fmt;
	logger->ctx = NULL;

	/* Ensure parent directory exists (fs_open creates files, not dirs). */
	strncpy(dir, path, sizeof(dir) - 1);
	dir[sizeof(dir) - 1] = '\0';
	sep = strrchr(dir, '/');
	if (sep != NULL && sep != dir) {
		*sep = '\0';
		fs_dir_t_init(&ptr);
		if (fs_opendir(&ptr, dir) < 0) {
			(void)fs_mkdir(dir);
		} else {
			fs_closedir(&ptr);
		}
	}

	rc = fmt->init(logger, path);

	if (rc != 0) {
		LOG_ERR("formatter init failed (%d)", rc);
		return rc;
	}

	rc = fmt->write_header(logger);
	if (rc != 0) {
		LOG_ERR("formatter write_header failed (%d)", rc);
		fmt->close(logger);
		return rc;
	}

	rc = fmt->flush(logger);
	if (rc != 0) {
		LOG_ERR("formatter flush after header failed (%d)", rc);
		fmt->close(logger);
		return rc;
	}

	return 0;
}

/* data_logger_log – see data_logger.h */
int data_logger_log(struct data_logger *logger, const struct datapoint *dp)
{
	if (logger == NULL || dp == NULL || logger->fmt == NULL)
		return -EINVAL;

	return logger->fmt->write_datapoint(logger, dp);
}

/* data_logger_flush – see data_logger.h */
int data_logger_flush(struct data_logger *logger)
{
	if (logger == NULL || logger->fmt == NULL)
		return -EINVAL;

	return logger->fmt->flush(logger);
}

/* data_logger_close – see data_logger.h */
int data_logger_close(struct data_logger *logger)
{
	if (logger == NULL || logger->fmt == NULL)
		return -EINVAL;

	int rc = logger->fmt->close(logger);

	logger->fmt = NULL;
	logger->ctx = NULL;

	return rc;
}
