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
/*  Logger registry                                                           */
/* -------------------------------------------------------------------------- */

static struct data_logger *registry[CONFIG_DATA_LOGGER_MAX_LOGGERS];

static int registry_add(struct data_logger *logger)
{
	for (int i = 0; i < CONFIG_DATA_LOGGER_MAX_LOGGERS; i++) {
		if (registry[i] == NULL) {
			registry[i] = logger;
			return 0;
		}
	}
	return -ENOMEM;
}

static void registry_remove(struct data_logger *logger)
{
	for (int i = 0; i < CONFIG_DATA_LOGGER_MAX_LOGGERS; i++) {
		if (registry[i] == logger) {
			registry[i] = NULL;
			return;
		}
	}
}

void data_logger_foreach(data_logger_cb_t cb, void *user_data)
{
	for (int i = 0; i < CONFIG_DATA_LOGGER_MAX_LOGGERS; i++) {
		if (registry[i] != NULL) {
			cb(registry[i], user_data);
		}
	}
}

struct data_logger *data_logger_get(const char *name)
{
	for (int i = 0; i < CONFIG_DATA_LOGGER_MAX_LOGGERS; i++) {
		if (registry[i] != NULL &&
		    strcmp(registry[i]->name, name) == 0) {
			return registry[i];
		}
	}
	return NULL;
}

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
int data_logger_init(struct data_logger *logger, const char *filename)
{
	struct fs_dir_t ptr;
	char dir[64];
	char *sep;
	int rc;
	struct data_logger_state *state = k_malloc(sizeof(*state));
#if defined(CONFIG_DATA_LOGGER_CSV)
	const struct data_logger_formatter *fmt = &data_logger_csv_formatter;
#elif defined(CONFIG_DATA_LOGGER_INFLUX)
	const struct data_logger_formatter *fmt = &data_logger_influx_formatter;
#elif defined(CONFIG_DATA_LOGGER_MOCK)
	const struct data_logger_formatter *fmt = &data_logger_mock_formatter;
#else
#error "Unknown data logger backend!"
#endif /* CONFIG_DATA_LOGGER */
	char full_path[64];

	if (logger == NULL || fmt == NULL || filename == NULL) {
		rc = -EINVAL;
		goto out_err;
	}

	/* Build "<base_path>/<filename>.<file_ext>" */
	rc = snprintf(full_path, sizeof(full_path), "%s/%s.%s",
		      CONFIG_DATA_LOGGER_BASE_PATH, filename, fmt->file_ext);
	if (rc < 0 || rc >= (int)sizeof(full_path)) {

		rc = -ENAMETOOLONG;
		goto out_err;
	}

	logger->fmt = fmt;
	logger->ctx = NULL;

	strncpy(logger->name, filename, sizeof(logger->name) - 1);
	logger->name[sizeof(logger->name) - 1] = '\0';

	logger->state = state;
	k_mutex_init(&logger->state->mutex);

	/* Ensure parent directory exists (fs_open creates files, not dirs). */
	strncpy(dir, full_path, sizeof(dir) - 1);
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

	rc = fmt->init(logger, full_path);

	if (rc != 0) {
		LOG_ERR("formatter init failed (%d)", rc);
		goto out_err;
	}

	rc = fmt->write_header(logger);
	if (rc != 0) {
		LOG_ERR("formatter write_header failed (%d)", rc);
		goto out_err_close;
	}

	rc = fmt->flush(logger);
	if (rc != 0) {
		LOG_ERR("formatter flush after header failed (%d)", rc);
		goto out_err_close;
	}

	rc = registry_add(logger);
	if (rc != 0) {
		LOG_ERR("logger registry full (%d)", rc);
		fmt->close(logger);
		return rc;
	}

	rc = data_logger_start(logger);
	if (rc) {
		LOG_ERR("logger starting failed (%d)", rc);
		goto out_err_close;
	}

	return 0;

out_err_close:
	fmt->close(logger);
out_err:
	k_free(state);
	return rc;
}

/* data_logger_log – see data_logger.h */
int data_logger_log(struct data_logger *logger, const struct datapoint *dp)
{
	if (logger == NULL || dp == NULL || logger->fmt == NULL ||
		logger->state == NULL)
		return -EINVAL;

	// silently ignore if not running
	if (logger->state->running == 0) {
		LOG_WRN("logger->state->running is 0");
		return 0;
	}

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
	int rc_lock, rc_close;

	if (logger == NULL || logger->fmt == NULL || logger->state == NULL)
		return -EINVAL;

	registry_remove(logger);

	rc_close = logger->fmt->close(logger);
	if (rc_close)
		LOG_ERR("formatter close failed (%d)", rc_close);

	rc_lock = k_mutex_lock(&logger->state->mutex, K_MSEC(500));
	if (rc_lock == 0) {
		k_free(logger->state);
	} else {
		LOG_ERR("formatter close could not lock state mutex (%d)",
			rc_lock);
		return rc_lock;
	}

	registry_remove(logger);

	logger->fmt = NULL;
	logger->ctx = NULL;
	logger->state = NULL;

	return rc_close;
}

/* data_logger_stop – see data_logger.h */
int data_logger_stop(struct data_logger *logger)
{
	int rc;

	if (logger == NULL || logger->fmt == NULL || logger->state == NULL)
		return -EINVAL;

	if (logger->fmt->stop != NULL) {
		rc = logger->fmt->stop(logger);
		if (rc) {
			LOG_ERR("formatter stop preparation failed (%d)", rc);
			return rc;
		}
	}

	rc = k_mutex_lock(&logger->state->mutex, K_MSEC(100));
	if (rc == 0) {
		logger->state->running = 0;
		k_mutex_unlock(&logger->state->mutex);
	} else {
		LOG_ERR("formatter stop could not lock state mutex (%d)", rc);
		return rc;
	}

	return data_logger_flush(logger);
}

/* data_logger_start – see data_logger.h */
int data_logger_start(struct data_logger *logger)
{
	int rc;

	if (logger == NULL || logger->fmt == NULL || logger->state == NULL)
		return -EINVAL;

	if (logger->fmt->start != NULL) {
		rc = logger->fmt->start(logger);
		if (rc) {
			LOG_ERR("formatter start preparation failed (%d)", rc);
			return rc;
		}
	}

	rc = k_mutex_lock(&logger->state->mutex, K_MSEC(100));
	if (rc == 0) {
		logger->state->running = 1;
		k_mutex_unlock(&logger->state->mutex);
	} else {
		LOG_ERR("formatter start could not lock state mutex (%d)", rc);
		return rc;
	}

	return 0;
}
