/**
 * @file fmt_csv.c
 * @brief CSV formatter backend for the Aurora data logger.
 *
 * Produces a UTF-8 CSV file with one header row followed by one data row per
 * logged datapoint.  All channel values are written with six decimal places in
 * the format @c val1.val2 (Zephyr sensor_value convention).
 *
 * Column layout:
 *   timestamp_ns, type,
 *   ch0_val1, ch0_val2, ch1_val1, ch1_val2, ch2_val1, ch2_val2
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
/*  Private context                                                           */
/* -------------------------------------------------------------------------- */

struct csv_ctx {
	struct fs_file_t file;
};

/* -------------------------------------------------------------------------- */
/*  Helpers                                                                   */
/* -------------------------------------------------------------------------- */

/**
 * Write a sensor_value as "integer.fraction" with six decimal places.
 * Both val1 and val2 share the same sign in Zephyr's convention, so we
 * print the minus sign once and use the absolute values of each part.
 */
static int write_sensor_value(struct fs_file_t *f,
			      const struct sensor_value *sv)
{
	char buf[24];
	int len;

	if (sv->val1 < 0 || sv->val2 < 0) {
		len = snprintf(buf, sizeof(buf), "-%d.%06d",
			       -(int)sv->val1, -(int)sv->val2);
	} else {
		len = snprintf(buf, sizeof(buf), "%d.%06d",
			       (int)sv->val1, (int)sv->val2);
	}

	if (len < 0 || len >= (int)sizeof(buf))
		return -ENOMEM;

	ssize_t wr = fs_write(f, buf, len);

	return wr < 0 ? (int)wr : 0;
}

/* -------------------------------------------------------------------------- */
/*  Formatter callbacks                                                       */
/* -------------------------------------------------------------------------- */

static int csv_init(struct data_logger *logger, const char *path)
{
	struct csv_ctx *ctx = k_malloc(sizeof(*ctx));

	if (!ctx)
		return -ENOMEM;

	fs_file_t_init(&ctx->file);

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

static int csv_write_header(struct data_logger *logger)
{
	struct csv_ctx *ctx = logger->ctx;
	const char *hdr =
		"timestamp_ns,type,"
		"ch0_val1,ch0_val2,"
		"ch1_val1,ch1_val2,"
		"ch2_val1,ch2_val2\n";

	ssize_t wr = fs_write(&ctx->file, hdr, strlen(hdr));

	return wr < 0 ? (int)wr : 0;
}

static int csv_write_datapoint(struct data_logger *logger,
			       const struct datapoint *dp)
{
	struct csv_ctx *ctx = logger->ctx;
	char buf[32];
	int len;
	ssize_t wr;
	int rc;

	/* timestamp_ns */
	len = snprintf(buf, sizeof(buf), "%" PRIu64 ",", dp->timestamp_ns);
	if (len < 0 || len >= (int)sizeof(buf))
		return -ENOMEM;

	wr = fs_write(&ctx->file, buf, len);
	if (wr < 0)
		return (int)wr;

	/* type name */
	const char *name = data_logger_type_name(dp->type);

	wr = fs_write(&ctx->file, name, strlen(name));
	if (wr < 0)
		return (int)wr;

	/* channel values — always write DP_MAX_CHANNELS columns */
	for (int i = 0; i < DP_MAX_CHANNELS; i++) {
		const struct sensor_value *sv =
			(i < dp->channel_count) ? &dp->channels[i] : NULL;

		if (sv) {
			rc = write_sensor_value(&ctx->file, sv);
			if (rc != 0)
				return rc;
		} else {
			/* pad with zeroes for unused channels */
			const char *zero = ",0.000000";

			wr = fs_write(&ctx->file, zero, strlen(zero));
			if (wr < 0)
				return (int)wr;

			continue;
		}

		/* comma separator after each channel */
		/* including last "," before "\n" */
		wr = fs_write(&ctx->file, ",", 1);
		if (wr < 0)
			return (int)wr;
	}

	wr = fs_write(&ctx->file, "\n", 1);
	return wr < 0 ? (int)wr : 0;
}

static int csv_flush(struct data_logger *logger)
{
	struct csv_ctx *ctx = logger->ctx;

	return fs_sync(&ctx->file);
}

static int csv_close(struct data_logger *logger)
{
	struct csv_ctx *ctx = logger->ctx;
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
