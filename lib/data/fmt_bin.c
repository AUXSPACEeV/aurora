/**
 * @file fmt_bin.c
 * @brief Fixed-size binary formatter backend for the Aurora data logger.
 *
 * Writes a 16-byte file header followed by one 40-byte record per datapoint.
 * This is the live flight-time formatter: small, fast, no floating-point
 * formatting. Post-flight the file is re-read record-by-record and
 * re-emitted through the CSV or InfluxDB formatter (see data_logger_convert).
 *
 * All multi-byte fields are stored in native little-endian layout; every
 * supported target (RP2040, RP2350, ESP32-S3, qemu_x86) is LE.
 *
 * Copyright (c) 2026 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>

#include <aurora/lib/data_logger.h>

LOG_MODULE_DECLARE(data_logger, CONFIG_DATA_LOGGER_LOG_LEVEL);

struct bin_ctx {
	struct fs_file_t file;
};

static int bin_init(struct data_logger *logger, const char *path)
{
	struct bin_ctx *ctx = k_malloc(sizeof(*ctx));

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

static int bin_write_header(struct data_logger *logger)
{
	struct bin_ctx *ctx = logger->ctx;
	struct aurora_bin_header hdr = {
		.magic        = AURORA_BIN_MAGIC,
		.version      = AURORA_BIN_VERSION,
		.record_size  = (uint16_t)sizeof(struct aurora_bin_record),
		.reserved     = 0,
	};

	ssize_t wr = fs_write(&ctx->file, &hdr, sizeof(hdr));

	return wr < 0 ? (int)wr : 0;
}

static int bin_write_datapoint(struct data_logger *logger,
			       const struct datapoint *dp)
{
	struct bin_ctx *ctx = logger->ctx;
	struct aurora_bin_record rec = {
		.timestamp_ns  = dp->timestamp_ns,
		.type          = (uint8_t)dp->type,
		.channel_count = dp->channel_count,
	};

	for (int i = 0; i < DP_MAX_CHANNELS; i++) {
		if (i < dp->channel_count) {
			rec.channels[i].val1 = dp->channels[i].val1;
			rec.channels[i].val2 = dp->channels[i].val2;
		}
	}

	ssize_t wr = fs_write(&ctx->file, &rec, sizeof(rec));

	return wr < 0 ? (int)wr : 0;
}

static int bin_flush(struct data_logger *logger)
{
	struct bin_ctx *ctx = logger->ctx;

	return fs_sync(&ctx->file);
}

static int bin_close(struct data_logger *logger)
{
	struct bin_ctx *ctx = logger->ctx;
	int rc = fs_close(&ctx->file);

	k_free(ctx);
	logger->ctx = NULL;

	return rc;
}

const struct data_logger_formatter data_logger_bin_formatter = {
	.init            = bin_init,
	.write_header    = bin_write_header,
	.write_datapoint = bin_write_datapoint,
	.flush           = bin_flush,
	.close           = bin_close,
	.file_ext        = "bin",
	.name            = "bin",
};
