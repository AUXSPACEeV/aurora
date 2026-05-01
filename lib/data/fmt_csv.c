/**
 * @file fmt_csv.c
 * @brief CSV formatter backend for the Aurora data logger.
 *
 * Produces a UTF-8 CSV file with one header row followed by one row per
 * cluster of samples whose timestamps fall within a small window
 * (CONFIG_DATA_LOGGER_CSV_WINDOW_NS) of the first sample in the cluster.
 * The flight loop emits all sensor groups (baro/accel/gyro/mag/sm_*) within
 * a few hundred microseconds, so one row holds one full snapshot across
 * sensors. Missing channels are left as empty cells.
 *
 * Column layout (sorted by type then field, matching tools/influx_to_csv.py):
 *   timestamp_ns, accel_x, accel_y, accel_z, baro_pres, baro_temp,
 *   gyro_x, gyro_y, gyro_z, mag_x, mag_y, mag_z,
 *   sm_kinematics_accel, sm_kinematics_accel_vert, sm_kinematics_velocity,
 *   sm_pose_altitude, sm_pose_orientation
 *
 * Sensor values are written as "integer.fraction" with six decimal places
 * (Zephyr sensor_value convention). The timestamp is the first sample in
 * the cluster, in nanoseconds.
 *
 * Copyright (c) 2026 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>

#include <aurora/lib/data_logger.h>

LOG_MODULE_DECLARE(data_logger, CONFIG_DATA_LOGGER_LOG_LEVEL);

#ifndef CONFIG_DATA_LOGGER_CSV_WINDOW_NS
#define CONFIG_DATA_LOGGER_CSV_WINDOW_NS 1000000
#endif

/* -------------------------------------------------------------------------- */
/*  Column table                                                              */
/* -------------------------------------------------------------------------- */

struct csv_column {
	enum aurora_data type;
	uint8_t channel;
	const char *name;
};

/* Columns are listed in the order they appear in the CSV header, which
 * matches tools/influx_to_csv.py: type names sorted alphabetically, fields
 * sorted alphabetically within each type.
 */
static const struct csv_column csv_columns[] = {
	{ AURORA_DATA_IMU_ACCEL,     0, "accel_x" },
	{ AURORA_DATA_IMU_ACCEL,     1, "accel_y" },
	{ AURORA_DATA_IMU_ACCEL,     2, "accel_z" },
	{ AURORA_DATA_BARO,          1, "baro_pres" },
	{ AURORA_DATA_BARO,          0, "baro_temp" },
	{ AURORA_DATA_IMU_GYRO,      0, "gyro_x" },
	{ AURORA_DATA_IMU_GYRO,      1, "gyro_y" },
	{ AURORA_DATA_IMU_GYRO,      2, "gyro_z" },
	{ AURORA_DATA_IMU_MAG,       0, "mag_x" },
	{ AURORA_DATA_IMU_MAG,       1, "mag_y" },
	{ AURORA_DATA_IMU_MAG,       2, "mag_z" },
	{ AURORA_DATA_SM_KINEMATICS, 0, "sm_kinematics_accel" },
	{ AURORA_DATA_SM_KINEMATICS, 1, "sm_kinematics_accel_vert" },
	{ AURORA_DATA_SM_KINEMATICS, 2, "sm_kinematics_velocity" },
	{ AURORA_DATA_SM_POSE,       1, "sm_pose_altitude" },
	{ AURORA_DATA_SM_POSE,       0, "sm_pose_orientation" },
};

#define CSV_COLUMN_COUNT ARRAY_SIZE(csv_columns)

static int column_index_for(enum aurora_data type, uint8_t channel)
{
	for (size_t i = 0; i < CSV_COLUMN_COUNT; i++) {
		if (csv_columns[i].type == type &&
		    csv_columns[i].channel == channel) {
			return (int)i;
		}
	}
	return -1;
}

/* -------------------------------------------------------------------------- */
/*  Private context                                                           */
/* -------------------------------------------------------------------------- */

struct csv_ctx {
	struct fs_file_t file;

	/* Pending row state. group_active is false before the first sample
	 * and after each flush_row().
	 */
	bool group_active;
	uint64_t group_start_ns;
	bool present[CSV_COLUMN_COUNT];
	struct sensor_value values[CSV_COLUMN_COUNT];

	/* RAM staging buffer for fs_write coalescing.  Pointer to the
	 * shared static buffer; @c used tracks how many bytes are pending.
	 */
	char *buf;
	size_t used;
};

/* -------------------------------------------------------------------------- */
/*  Helpers                                                                   */
/* -------------------------------------------------------------------------- */

static char csv_write_buf[CONFIG_DATA_LOGGER_CSV_BUF_SIZE];

static int buf_drain(struct csv_ctx *ctx)
{
	if (ctx->used == 0) {
		return 0;
	}
	ssize_t wr = fs_write(&ctx->file, ctx->buf, ctx->used);

	if (wr < 0) {
		return (int)wr;
	}
	ctx->used = 0;
	return 0;
}

static int write_all(struct csv_ctx *ctx, const char *src, size_t len)
{
	if (len >= CONFIG_DATA_LOGGER_CSV_BUF_SIZE) {
		int rc = buf_drain(ctx);

		if (rc != 0) {
			return rc;
		}
		ssize_t wr = fs_write(&ctx->file, src, len);

		return wr < 0 ? (int)wr : 0;
	}
	if (ctx->used + len > CONFIG_DATA_LOGGER_CSV_BUF_SIZE) {
		int rc = buf_drain(ctx);

		if (rc != 0) {
			return rc;
		}
	}
	memcpy(ctx->buf + ctx->used, src, len);
	ctx->used += len;
	return 0;
}

static int format_sensor_value(char *buf, size_t bufsz,
			       const struct sensor_value *sv)
{
	if (sv->val1 < 0 || sv->val2 < 0) {
		return snprintf(buf, bufsz, "-%d.%06d",
				-(int)sv->val1, -(int)sv->val2);
	}
	return snprintf(buf, bufsz, "%d.%06d",
			(int)sv->val1, (int)sv->val2);
}

static int flush_row(struct csv_ctx *ctx)
{
	if (!ctx->group_active) {
		return 0;
	}

	char buf[32];
	int len;
	int rc;

	len = snprintf(buf, sizeof(buf), "%" PRIu64, ctx->group_start_ns);
	if (len < 0 || len >= (int)sizeof(buf)) {
		return -ENOMEM;
	}
	rc = write_all(ctx, buf, len);
	if (rc != 0) {
		return rc;
	}

	for (size_t i = 0; i < CSV_COLUMN_COUNT; i++) {
		rc = write_all(ctx, ",", 1);
		if (rc != 0) {
			return rc;
		}
		if (ctx->present[i]) {
			len = format_sensor_value(buf, sizeof(buf),
						  &ctx->values[i]);
			if (len < 0 || len >= (int)sizeof(buf)) {
				return -ENOMEM;
			}
			rc = write_all(ctx, buf, len);
			if (rc != 0) {
				return rc;
			}
		}
	}

	rc = write_all(ctx, "\n", 1);
	if (rc != 0) {
		return rc;
	}

	ctx->group_active = false;
	memset(ctx->present, 0, sizeof(ctx->present));
	return 0;
}

/* -------------------------------------------------------------------------- */
/*  Formatter callbacks                                                       */
/* -------------------------------------------------------------------------- */

static int csv_init(struct data_logger *logger, const char *path)
{
	struct csv_ctx *ctx = k_calloc(1, sizeof(*ctx));

	if (!ctx) {
		return -ENOMEM;
	}

	fs_file_t_init(&ctx->file);

	int rc = fs_open(&ctx->file, path,
			 FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);

	if (rc != 0) {
		LOG_ERR("failed to open %s (%d)", path, rc);
		k_free(ctx);
		return rc;
	}

	ctx->buf = csv_write_buf;
	ctx->used = 0;
	logger->ctx = ctx;
	return 0;
}

static int csv_write_header(struct data_logger *logger)
{
	struct csv_ctx *ctx = logger->ctx;
	int rc = write_all(ctx, "timestamp_ns", strlen("timestamp_ns"));

	if (rc != 0) {
		return rc;
	}

	for (size_t i = 0; i < CSV_COLUMN_COUNT; i++) {
		rc = write_all(ctx, ",", 1);
		if (rc != 0) {
			return rc;
		}
		rc = write_all(ctx, csv_columns[i].name,
			       strlen(csv_columns[i].name));
		if (rc != 0) {
			return rc;
		}
	}

	return write_all(ctx, "\n", 1);
}

static int csv_write_datapoint(struct data_logger *logger,
			       const struct datapoint *dp)
{
	struct csv_ctx *ctx = logger->ctx;
	int rc;

	/* Close out the current row if this datapoint falls outside the
	 * grouping window. Comparing as uint64_t handles the (unexpected)
	 * case of timestamps moving backwards by wrapping to a huge delta,
	 * which also forces a new group.
	 */
	if (ctx->group_active &&
	    dp->timestamp_ns - ctx->group_start_ns >
	    (uint64_t)CONFIG_DATA_LOGGER_CSV_WINDOW_NS) {
		rc = flush_row(ctx);
		if (rc != 0) {
			return rc;
		}
	}

	if (!ctx->group_active) {
		ctx->group_start_ns = dp->timestamp_ns;
		ctx->group_active = true;
	}

	for (uint8_t ch = 0; ch < dp->channel_count && ch < DP_MAX_CHANNELS;
	     ch++) {
		int col = column_index_for(dp->type, ch);

		if (col < 0) {
			continue;
		}
		ctx->values[col] = dp->channels[ch];
		ctx->present[col] = true;
	}

	return 0;
}

static int csv_flush(struct data_logger *logger)
{
	struct csv_ctx *ctx = logger->ctx;
	int rc = flush_row(ctx);

	if (rc != 0) {
		return rc;
	}
	rc = buf_drain(ctx);
	if (rc != 0) {
		return rc;
	}
	return fs_sync(&ctx->file);
}

static int csv_close(struct data_logger *logger)
{
	struct csv_ctx *ctx = logger->ctx;

	(void)flush_row(ctx);
	(void)buf_drain(ctx);

	int rc = fs_close(&ctx->file);

	k_free(ctx);
	logger->ctx = NULL;

	return rc;
}

/* -------------------------------------------------------------------------- */
/*  Exported formatter                                                        */
/* -------------------------------------------------------------------------- */

const struct data_logger_formatter data_logger_csv_formatter = {
	.init            = csv_init,
	.write_header    = csv_write_header,
	.write_datapoint = csv_write_datapoint,
	.flush           = csv_flush,
	.close           = csv_close,
	.file_ext        = "csv",
	.name            = "csv",
};
