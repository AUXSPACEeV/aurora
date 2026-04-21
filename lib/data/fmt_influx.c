/**
 * @file fmt_influx.c
 * @brief InfluxDB Line Protocol formatter backend for the Aurora data logger.
 *
 * Each datapoint is written as one InfluxDB line:
 *
 *   [measurement,type=name field=<val[,field=val...] timestamp_ms
 *
 * Example:
 *   telemetry,type=baro temp=23.500000,pres=101325.000000 1700000000000
 *
 * There is no header row; the measurement name is taken from
 * CONFIG_DATA_LOGGER_INFLUX_MEASUREMENT (default: "telemetry").
 *
 * Field names per sensor group:
 *   baro      → temp, pres
 *   accel → x, y, z
 *   gyro  → x, y, z
 *   mag   → x, y, z
 *
 * Copyright (c) 2026 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>

#include <aurora/lib/data_logger.h>

LOG_MODULE_DECLARE(data_logger, CONFIG_DATA_LOGGER_LOG_LEVEL);

/* -------------------------------------------------------------------------- */
/*  Field-name table                                                          */
/* -------------------------------------------------------------------------- */

static const char *field_names_for(enum aurora_data type, int channel)
{
	static const char *const baro_fields[]  = {"temp",
						   "pres", NULL};
	static const char *const xyz_fields[]   = {"x", "y", "z"};

	switch (type) {
	case AURORA_DATA_BARO:
		return (channel < 2) ? baro_fields[channel] : "unknown";
	case AURORA_DATA_IMU_ACCEL:
	case AURORA_DATA_IMU_GYRO:
	case AURORA_DATA_IMU_MAG:
		return (channel < 3) ? xyz_fields[channel] : "unknown";
	default:
		return "unknown";
	}
}

/* -------------------------------------------------------------------------- */
/*  Private context                                                           */
/* -------------------------------------------------------------------------- */

static char influx_write_buf[CONFIG_DATA_LOGGER_INFLUX_BUF_SIZE];

struct influx_ctx {
	struct fs_file_t file;
	char *buf;
	size_t used;
};

/* -------------------------------------------------------------------------- */
/*  Formatter callbacks                                                       */
/* -------------------------------------------------------------------------- */

static int influx_init(struct data_logger *logger, const char *path)
{
	struct influx_ctx *ctx = k_malloc(sizeof(*ctx));

	if (!ctx)
		return -ENOMEM;

	fs_file_t_init(&ctx->file);
	ctx->buf = influx_write_buf;
	ctx->used = 0;

	int rc = fs_open(&ctx->file, path,
			 FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);

	if (rc != 0) {
		LOG_ERR("failed to open %s (%d)", path, rc);
		k_free(ctx);
		return rc;
	}

	logger->ctx = ctx;
	return 0;
}

static int influx_write_header(struct data_logger *logger)
{
	/* InfluxDB line protocol has no header */
	(void)logger;
	return 0;
}

static int influx_format_line(char *dst, size_t dst_size,
			      const struct datapoint *dp)
{
	int offset = 0;
	int n;

	/* measurement,type=<name> */
	n = snprintf(dst + offset, dst_size - offset,
		     "%s,type=%s ",
		     CONFIG_DATA_LOGGER_INFLUX_MEASUREMENT,
		     data_logger_type_name(dp->type));
	if (n < 0 || n >= (int)(dst_size - offset)) {
		return -ENOMEM;
	}
	offset += n;

	/* fields: name=val1.val2 (comma-separated, six decimal places) */
	for (int i = 0; i < dp->channel_count && i < DP_MAX_CHANNELS; i++) {
		const struct sensor_value *sv = &dp->channels[i];
		const char *fname = field_names_for(dp->type, i);
		int32_t v1 = sv->val1;
		int32_t v2 = sv->val2;

		if (v1 < 0 || v2 < 0) {
			n = snprintf(dst + offset, dst_size - offset,
				     "%s%s=-%d.%06d",
				     i > 0 ? "," : "",
				     fname, -v1, -v2);
		} else {
			n = snprintf(dst + offset, dst_size - offset,
				     "%s%s=%d.%06d",
				     i > 0 ? "," : "",
				     fname, v1, v2);
		}

		if (n < 0 || n >= (int)(dst_size - offset)) {
			return -ENOMEM;
		}

		offset += n;
	}

	/* timestamp in milliseconds */
	n = snprintf(dst + offset, dst_size - offset,
		     " %" PRId64 "\n", dp->timestamp_ms);
	if (n < 0 || n >= (int)(dst_size - offset)) {
		return -ENOMEM;
	}
	offset += n;

	return offset;
}

static int influx_write_datapoint(struct data_logger *logger,
				  const struct datapoint *dp)
{
	struct influx_ctx *ctx = logger->ctx;
	char line[256];

	int len = influx_format_line(line, sizeof(line), dp);

	if (len < 0) {
		LOG_WRN("influx line truncated for type %s",
			data_logger_type_name(dp->type));
		return len;
	}

	/* Flush the RAM buffer to disk if the new line would not fit */
	if (ctx->used + len > CONFIG_DATA_LOGGER_INFLUX_BUF_SIZE) {
		ssize_t wr = fs_write(&ctx->file, ctx->buf, ctx->used);

		if (wr < 0) {
			return (int)wr;
		}
		ctx->used = 0;
	}

	/* If a single line exceeds the entire buffer, write it directly */
	if (len > (int)CONFIG_DATA_LOGGER_INFLUX_BUF_SIZE) {
		ssize_t wr = fs_write(&ctx->file, line, len);

		return wr < 0 ? (int)wr : 0;
	}

	memcpy(ctx->buf + ctx->used, line, len);
	ctx->used += len;
	return 0;
}

static int influx_flush(struct data_logger *logger)
{
	struct influx_ctx *ctx = logger->ctx;

	if (ctx->used > 0) {
		ssize_t wr = fs_write(&ctx->file, ctx->buf, ctx->used);

		if (wr < 0) {
			return (int)wr;
		}
		ctx->used = 0;
	}

	return fs_sync(&ctx->file);
}

static int influx_close(struct data_logger *logger)
{
	struct influx_ctx *ctx = logger->ctx;

	/* Drain remaining buffered data before closing */
	if (ctx->used > 0) {
		fs_write(&ctx->file, ctx->buf, ctx->used);
		ctx->used = 0;
	}

	int rc = fs_close(&ctx->file);

	k_free(ctx);
	logger->ctx = NULL;

	return rc;
}

/* -------------------------------------------------------------------------- */
/*  Exported formatter                                                        */
/* -------------------------------------------------------------------------- */

const struct data_logger_formatter data_logger_influx_formatter = {
	.init            = influx_init,
	.write_header    = influx_write_header,
	.write_datapoint = influx_write_datapoint,
	.flush           = influx_flush,
	.close           = influx_close,
	.file_ext        = "influx",
	.name            = "influx",
};
