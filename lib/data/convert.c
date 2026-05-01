/**
 * @file convert.c
 * @brief Streaming converter: binary log → text formatter output.
 *
 * Walks the live flight log on whichever backend the bin formatter is
 * configured against (flash partition or disk-access region — see
 * bin_io.h) frame-by-frame, reconstructs each @ref datapoint losslessly,
 * and feeds it through the target formatter (CSV or InfluxDB).
 *
 * Recovery algorithm (works for both circular flash and linear disk):
 *
 *   1. Header-scan every slot to find the latest @c flight_id and,
 *      among its frames, the slot holding the lowest @c seq — that's
 *      the oldest surviving frame in the captured window.  For a
 *      linear-from-zero writer this is just slot 0; for a circular
 *      writer it can be anywhere in the partition.
 *   2. Walk forward in physical order from that slot, wrapping at the
 *      region end if needed, expecting @c seq to increment by exactly
 *      1 per step.  Stop on the first slot that doesn't match
 *      (different flight, bad magic, gap, or out-of-order seq).
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
#include <zephyr/logging/log.h>

#include <aurora/lib/data_logger.h>

#include "bin_io.h"

LOG_MODULE_DECLARE(data_logger, CONFIG_DATA_LOGGER_LOG_LEVEL);

#define BIN_FRAME_SIZE       ((size_t)CONFIG_DATA_LOGGER_BIN_FRAME_SIZE)
#define BIN_HDR_SIZE         (sizeof(struct aurora_bin_frame_header))

/* One frame's worth of bytes; aligned for the underlying driver. */
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

/* Read a full frame at @p off into the static convert_frame buffer.
 * Backends that prefer header-only reads may still treat this as a
 * full-frame read; the cost is one extra page per slot during the scan
 * pass, which is bounded.
 */
static int read_frame(off_t off)
{
	int rc = bin_io_read(off, convert_frame, BIN_FRAME_SIZE);

	if (rc != 0) {
		LOG_ERR("convert: bin_io_read at %ld failed (%d)",
			(long)off, rc);
	}
	return rc;
}

/* Returns 0 if the slot at @p off holds a valid current-version frame
 * (header copied to *out), positive if the slot is unwritten /
 * unsupported, negative on read error.
 */
static int read_header(off_t off, struct aurora_bin_frame_header *out)
{
	int rc = read_frame(off);

	if (rc != 0) {
		return rc;
	}

	memcpy(out, convert_frame, sizeof(*out));

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

/* Pick the start of the captured window: the frame with the highest
 * flight_id and, within that flight, the lowest seq.
 */
static int find_window_start(size_t total_size,
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
	     (size_t)off + BIN_FRAME_SIZE <= total_size;
	     off += (off_t)BIN_FRAME_SIZE) {
		int rc = read_header(off, &fh);

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
	struct data_logger out_logger;
	struct data_logger_state out_state;
	off_t start_offset;
	uint32_t expect_seq;
	uint64_t flight_id;
	int rc;

	if (out_fmt == NULL || out_path == NULL) {
		return -EINVAL;
	}

	rc = bin_io_open();
	if (rc != 0) {
		return rc;
	}

	const size_t total_size = bin_io_total_size();
	const uint32_t ring_frames =
		(uint32_t)(total_size / BIN_FRAME_SIZE);

	memset(&out_logger, 0, sizeof(out_logger));
	memset(&out_state, 0, sizeof(out_state));
	k_mutex_init(&out_state.mutex);
	out_logger.fmt   = out_fmt;
	out_logger.state = &out_state;

	rc = out_fmt->init(&out_logger, out_path);
	if (rc != 0) {
		LOG_ERR("convert: %s init failed (%d)", out_fmt->name, rc);
		goto out_io;
	}

	rc = out_fmt->write_header(&out_logger);
	if (rc != 0) {
		LOG_ERR("convert: %s write_header failed (%d)",
			out_fmt->name, rc);
		goto out_close;
	}

	rc = bin_io_window_start_hint(&start_offset, &expect_seq, &flight_id);
	if (rc == -ENOTSUP) {
		rc = find_window_start(total_size, &start_offset, &expect_seq,
				       &flight_id);
	}
	if (rc == -ENOENT) {
		/* No valid frames; emit an empty (header-only) file
		 * successfully so callers can distinguish "no flight" from
		 * real errors via on-disk presence.
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

	while (frames_seen < ring_frames) {
		rc = read_frame(cur_offset);
		if (rc != 0) {
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
		if ((size_t)cur_offset >= total_size) {
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
out_io:
	(void)bin_io_close();
	return rc;
}
