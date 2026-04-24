/**
 * @file convert.c
 * @brief Streaming converter: binary log → text formatter output.
 *
 * Reads a binary log file produced by fmt_bin.c a fixed-size chunk at a
 * time, reconstructs a @ref datapoint from each 40-byte record, and feeds
 * it through the target formatter (CSV or InfluxDB).  Memory usage is
 * bounded — no matter how large the binary log is, only one small chunk
 * is held in RAM at any time.
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

#define CONVERT_CHUNK_RECORDS 16

static void record_to_datapoint(const struct aurora_bin_record *rec,
				struct datapoint *dp)
{
	dp->timestamp_ns  = rec->timestamp_ns;
	dp->type          = (enum aurora_data)rec->type;
	dp->channel_count = rec->channel_count;

	for (int i = 0; i < DP_MAX_CHANNELS; i++) {
		dp->channels[i].val1 = rec->channels[i].val1;
		dp->channels[i].val2 = rec->channels[i].val2;
	}
}

int data_logger_convert(const char *bin_path,
			const struct data_logger_formatter *out_fmt,
			const char *out_path)
{
	struct fs_file_t in;
	struct aurora_bin_header hdr;
	struct data_logger out_logger;
	struct data_logger_state out_state;
	struct aurora_bin_record chunk[CONVERT_CHUNK_RECORDS];
	ssize_t n;
	int rc;

	if (bin_path == NULL || out_fmt == NULL || out_path == NULL)
		return -EINVAL;

	fs_file_t_init(&in);
	rc = fs_open(&in, bin_path, FS_O_READ);
	if (rc != 0) {
		LOG_ERR("convert: cannot open %s (%d)", bin_path, rc);
		return rc;
	}

	n = fs_read(&in, &hdr, sizeof(hdr));
	if (n != (ssize_t)sizeof(hdr)) {
		LOG_ERR("convert: short header read from %s (%zd)", bin_path, n);
		rc = (n < 0) ? (int)n : -EIO;
		goto out_in;
	}

	if (memcmp(hdr.magic, AURORA_BIN_MAGIC, sizeof(hdr.magic)) != 0) {
		LOG_ERR("convert: bad magic in %s", bin_path);
		rc = -EBADF;
		goto out_in;
	}

	if (hdr.version != AURORA_BIN_VERSION ||
	    hdr.record_size != sizeof(struct aurora_bin_record)) {
		LOG_ERR("convert: unsupported binary format v=%u size=%u",
			hdr.version, hdr.record_size);
		rc = -ENOTSUP;
		goto out_in;
	}

	memset(&out_logger, 0, sizeof(out_logger));
	memset(&out_state, 0, sizeof(out_state));
	k_mutex_init(&out_state.mutex);
	out_logger.fmt   = out_fmt;
	out_logger.state = &out_state;

	rc = out_fmt->init(&out_logger, out_path);
	if (rc != 0) {
		LOG_ERR("convert: %s init failed (%d)", out_fmt->name, rc);
		goto out_in;
	}

	rc = out_fmt->write_header(&out_logger);
	if (rc != 0) {
		LOG_ERR("convert: %s write_header failed (%d)", out_fmt->name, rc);
		goto out_fmt_close;
	}

	while (1) {
		n = fs_read(&in, chunk, sizeof(chunk));
		if (n < 0) {
			rc = (int)n;
			goto out_fmt_close;
		}
		if (n == 0)
			break;

		/* Reject trailing partial record (truncated file) */
		size_t full = (size_t)n / sizeof(struct aurora_bin_record);

		for (size_t i = 0; i < full; i++) {
			struct datapoint dp;

			record_to_datapoint(&chunk[i], &dp);
			rc = out_fmt->write_datapoint(&out_logger, &dp);
			if (rc != 0) {
				LOG_ERR("convert: write_datapoint failed (%d)",
					rc);
				goto out_fmt_close;
			}
		}

		if ((size_t)n % sizeof(struct aurora_bin_record) != 0) {
			LOG_WRN("convert: %s ends with truncated record",
				bin_path);
			break;
		}
	}

	rc = out_fmt->flush(&out_logger);
	if (rc != 0)
		LOG_ERR("convert: flush failed (%d)", rc);

out_fmt_close:
	{
		int rc_close = out_fmt->close(&out_logger);

		if (rc == 0 && rc_close != 0)
			rc = rc_close;
	}
out_in:
	(void)fs_close(&in);
	return rc;
}
