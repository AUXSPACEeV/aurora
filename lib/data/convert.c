/**
 * @file convert.c
 * @brief Streaming converter: raw-flash binary log → text formatter output.
 *
 * Walks the live flight log on the @c auxspace,flight-log flash partition
 * frame-by-frame, reconstructs each @ref datapoint losslessly, and feeds
 * it through the target formatter (CSV or InfluxDB).
 *
 * The on-flash partition is a ring; the writer fills it linearly and
 * wraps at the partition end.  After @c on_event(DLE_BOOST) it caps
 * itself at @c ring_frames post-boost commits, which leaves either the
 * BOOST frame's slot at the next-write position or a gap from the
 * post-cap drops.  To recover the captured window we therefore:
 *
 *   1. Header-scan every slot to find the latest @c flight_id and, among
 *      its frames, the slot holding the lowest @c seq — that's the
 *      oldest surviving frame in the ring (the start of the captured
 *      window, i.e. the first pre-boost padding frame).
 *   2. Walk forward in physical order from that slot, wrapping at the
 *      partition end, expecting @c seq to increment by exactly 1 per
 *      step.  Stop on the first slot that doesn't match (different
 *      flight, bad magic, gap, or out-of-order seq).
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
#define BIN_HDR_SIZE         (sizeof(struct aurora_bin_frame_header))

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

/* Read just the frame header from a slot. Returns 0 if the slot holds a
 * valid (matching-version) frame; positive if the slot is unwritten or
 * holds non-current-version data; negative on flash error.
 */
static int read_header(const struct flash_area *fa, off_t off,
		       struct aurora_bin_frame_header *out)
{
	int rc = flash_area_read(fa, off, out, sizeof(*out));

	if (rc != 0) {
		LOG_ERR("convert: flash_area_read header at %ld failed (%d)",
			(long)off, rc);
		return rc;
	}

	if (memcmp(out->magic, AURORA_BIN_FRAME_MAGIC,
		   sizeof(out->magic)) != 0) {
		return 1; /* unwritten / non-frame */
	}

	if (out->version != AURORA_BIN_VERSION) {
		LOG_WRN("convert: unsupported frame version %u at %ld",
			out->version, (long)off);
		return 1;
	}

	return 0;
}

/* Scan every slot once and pick the start of the captured window: the
 * frame with the highest flight_id and, within that flight, the lowest
 * seq. Returns 0 and sets *out_offset / *out_seq / *out_flight_id on
 * success; -ENOENT if the partition holds no valid frames; negative
 * errno on flash error.
 */
static int find_window_start(const struct flash_area *fa,
			     off_t *out_offset,
			     uint32_t *out_seq,
			     uint64_t *out_flight_id)
{
	struct aurora_bin_frame_header fh;
	uint64_t max_flight_id = 0;
	uint32_t min_seq_in_max = UINT32_MAX;
	off_t start_offset = -1;
	bool any = false;

	for (off_t off = 0;
	     (size_t)off + BIN_FRAME_SIZE <= (size_t)BIN_FLASH_AREA_SIZE;
	     off += (off_t)BIN_FRAME_SIZE) {
		int rc = read_header(fa, off, &fh);

		if (rc < 0) {
			return rc;
		}
		if (rc > 0) {
			continue;
		}

		if (!any || fh.flight_id > max_flight_id) {
			max_flight_id  = fh.flight_id;
			min_seq_in_max = fh.seq;
			start_offset   = off;
			any            = true;
		} else if (fh.flight_id == max_flight_id &&
			   fh.seq < min_seq_in_max) {
			min_seq_in_max = fh.seq;
			start_offset   = off;
		}
	}

	if (!any) {
		return -ENOENT;
	}

	*out_offset    = start_offset;
	*out_seq       = min_seq_in_max;
	*out_flight_id = max_flight_id;
	return 0;
}

int data_logger_convert(const struct data_logger_formatter *out_fmt,
			const char *out_path)
{
	const struct flash_area *fa;
	struct data_logger out_logger;
	struct data_logger_state out_state;
	off_t start_offset;
	uint32_t expect_seq;
	uint64_t flight_id;
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

	rc = find_window_start(fa, &start_offset, &expect_seq, &flight_id);
	if (rc == -ENOENT) {
		/* Partition is virgin or holds no valid frames; emit an empty
		 * file successfully so callers can distinguish "no flight"
		 * from real errors via on-disk presence.
		 */
		rc = 0;
		goto out_flush;
	}
	if (rc != 0) {
		goto out_close;
	}

	const size_t records_per_frame =
		(BIN_FRAME_SIZE - BIN_HDR_SIZE) /
		sizeof(struct aurora_bin_record);

	off_t cur_offset = start_offset;
	uint32_t frames_seen = 0;
	const uint32_t ring_frames =
		(uint32_t)(BIN_FLASH_AREA_SIZE / BIN_FRAME_SIZE);

	while (frames_seen < ring_frames) {
		rc = flash_area_read(fa, cur_offset, convert_frame,
				     BIN_FRAME_SIZE);
		if (rc != 0) {
			LOG_ERR("convert: flash_area_read at %ld failed (%d)",
				(long)cur_offset, rc);
			goto out_close;
		}

		const struct aurora_bin_frame_header *fh =
			(const struct aurora_bin_frame_header *)convert_frame;

		if (memcmp(fh->magic, AURORA_BIN_FRAME_MAGIC,
			   sizeof(fh->magic)) != 0) {
			break; /* gap (e.g. boost cap region) */
		}
		if (fh->flight_id != flight_id) {
			break; /* leftover from a previous flight */
		}
		if (fh->seq != expect_seq) {
			break; /* out-of-order / skipped */
		}

		const struct aurora_bin_record *recs =
			(const struct aurora_bin_record *)
			(convert_frame + BIN_HDR_SIZE);

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

		expect_seq++;
		frames_seen++;
		cur_offset += (off_t)BIN_FRAME_SIZE;
		if ((size_t)cur_offset >= (size_t)BIN_FLASH_AREA_SIZE) {
			cur_offset = 0;
		}
	}

out_flush:
	{
		int rc_flush = out_fmt->flush(&out_logger);

		if (rc == 0 && rc_flush != 0) {
			LOG_ERR("convert: flush failed (%d)", rc_flush);
			rc = rc_flush;
		}
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
