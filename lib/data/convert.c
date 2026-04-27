/**
 * @file convert.c
 * @brief Streaming converter: raw-flash binary log → text formatter output.
 *
 * Walks the live flight log on the @c auxspace,flight-log flash partition
 * frame-by-frame, reconstructs each @ref datapoint losslessly, and feeds it
 * through the target formatter (CSV or InfluxDB).  Reading stops at the
 * first frame whose magic is invalid (== unwritten flash) or whose
 * @c flight_id differs from the first frame's — that's how the boundary
 * between the current flight and any leftover data on the partition is
 * detected.
 *
 * Memory usage is bounded: only one frame and one record-decoded
 * @ref datapoint are held in RAM at a time, regardless of log length.
 *
 * Copyright (c) 2026 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/logging/log.h>

#include <aurora/lib/data_logger.h>

LOG_MODULE_DECLARE(data_logger, CONFIG_DATA_LOGGER_LOG_LEVEL);

#if !DT_HAS_CHOSEN(auxspace_flight_log)
#error "data_logger_convert needs DT chosen 'auxspace,flight-log'."
#endif

#define BIN_FLASH_AREA_ID    DT_FIXED_PARTITION_ID(DT_CHOSEN(auxspace_flight_log))
#define BIN_FLASH_AREA_SIZE  DT_REG_SIZE(DT_CHOSEN(auxspace_flight_log))
#define BIN_FRAME_SIZE       ((size_t)CONFIG_DATA_LOGGER_BIN_FRAME_SIZE)

/* One frame's worth of bytes; aligned for the flash driver. */
static uint8_t convert_frame[CONFIG_DATA_LOGGER_BIN_FRAME_SIZE]
	__aligned(CONFIG_DATA_LOGGER_BIN_BUF_ALIGN);

static void record_to_datapoint(const struct aurora_bin_record *rec,
				uint64_t base_ts_ns,
				struct datapoint *dp)
{
	dp->timestamp_ns  = base_ts_ns +
			    (uint64_t)rec->ts_delta_us * 1000ULL;
	dp->type          = (enum aurora_data)rec->type;
	dp->channel_count = rec->channel_count;

	for (int i = 0; i < DP_MAX_CHANNELS; i++) {
		dp->channels[i].val1 = rec->channels[i].val1;
		dp->channels[i].val2 = rec->channels[i].val2;
	}
}

int data_logger_convert(const struct data_logger_formatter *out_fmt,
			const char *out_path)
{
	const struct flash_area *fa;
	struct data_logger out_logger;
	struct data_logger_state out_state;
	uint64_t flight_id = 0;
	bool flight_id_set = false;
	off_t offset = 0;
	int rc;

	if (out_fmt == NULL || out_path == NULL) {
		return -EINVAL;
	}

	rc = flash_area_open(BIN_FLASH_AREA_ID, &fa);
	if (rc != 0) {
		LOG_ERR("convert: flash_area_open failed (%d)", rc);
		return rc;
	}

	memset(&out_logger, 0, sizeof(out_logger));
	memset(&out_state, 0, sizeof(out_state));
	k_mutex_init(&out_state.mutex);
	out_logger.fmt   = out_fmt;
	out_logger.state = &out_state;

	rc = out_fmt->init(&out_logger, out_path);
	if (rc != 0) {
		LOG_ERR("convert: %s init failed (%d)", out_fmt->name, rc);
		goto out_fa;
	}

	rc = out_fmt->write_header(&out_logger);
	if (rc != 0) {
		LOG_ERR("convert: %s write_header failed (%d)",
			out_fmt->name, rc);
		goto out_close;
	}

	const size_t records_per_frame =
		(BIN_FRAME_SIZE - sizeof(struct aurora_bin_frame_header)) /
		sizeof(struct aurora_bin_record);

	while ((size_t)offset + BIN_FRAME_SIZE <= (size_t)BIN_FLASH_AREA_SIZE) {
		rc = flash_area_read(fa, offset, convert_frame, BIN_FRAME_SIZE);
		if (rc != 0) {
			LOG_ERR("convert: flash_area_read at %ld failed (%d)",
				(long)offset, rc);
			goto out_close;
		}

		const struct aurora_bin_frame_header *fh =
			(const struct aurora_bin_frame_header *)convert_frame;

		if (memcmp(fh->magic, AURORA_BIN_FRAME_MAGIC,
			   sizeof(fh->magic)) != 0) {
			/* Unwritten / non-frame data → end of log. */
			break;
		}

		if (fh->version != AURORA_BIN_VERSION) {
			LOG_ERR("convert: unsupported frame version %u at %ld",
				fh->version, (long)offset);
			rc = -ENOTSUP;
			goto out_close;
		}

		if (!flight_id_set) {
			flight_id = fh->flight_id;
			flight_id_set = true;
		} else if (fh->flight_id != flight_id) {
			/* Reached frames belonging to a previous flight. */
			break;
		}

		const struct aurora_bin_record *recs =
			(const struct aurora_bin_record *)
			(convert_frame + sizeof(*fh));

		for (size_t i = 0; i < records_per_frame; i++) {
			const struct aurora_bin_record *rec = &recs[i];

			/* type=0xFF means the slot is unwritten flash;
			 * type >= AURORA_DATA_COUNT is corruption.
			 */
			if (rec->type == 0xFFU) {
				break;
			}
			if (rec->type >= AURORA_DATA_COUNT) {
				LOG_WRN("convert: invalid record type %u at "
					"frame %u, slot %zu — stopping frame",
					rec->type, fh->seq, i);
				break;
			}

			struct datapoint dp;

			record_to_datapoint(rec, fh->base_ts_ns, &dp);

			rc = out_fmt->write_datapoint(&out_logger, &dp);
			if (rc != 0) {
				LOG_ERR("convert: write_datapoint failed (%d)",
					rc);
				goto out_close;
			}
		}

		offset += BIN_FRAME_SIZE;
	}

	rc = out_fmt->flush(&out_logger);
	if (rc != 0) {
		LOG_ERR("convert: flush failed (%d)", rc);
	}

out_close:
	{
		int rc_close = out_fmt->close(&out_logger);

		if (rc == 0 && rc_close != 0) {
			rc = rc_close;
		}
	}
out_fa:
	flash_area_close(fa);
	return rc;
}
