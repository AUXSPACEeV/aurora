/**
 * @file fmt_bin_fs.c
 * @brief Binary flight-log backend that writes through the filesystem.
 *
 * Same on-storage frame format as the flash and raw-disk backends -- a
 * sequence of BIN_FRAME_SIZE frames, each a 32-byte
 * @ref aurora_bin_frame_header followed by densely packed 32-byte
 * @ref aurora_bin_record entries -- but the frames land in an ordinary file
 * instead of a raw region.  convert.c reads it through the same bin_io
 * surface and cannot tell the difference.
 *
 * Why this exists: with the raw-region backend the flight is only readable
 * after a post-flight conversion pass.  Anything that stops the board before
 * that pass runs -- a watchdog reset, a freeze, a flat pack -- leaves the
 * data stranded in a region no host tool mounts.  Here the flight is a file
 * on the FAT volume the moment each frame is written, so pulling the card is
 * enough and conversion becomes an optional convenience
 * (CONFIG_DATA_LOGGER_CONVERT).
 *
 * Deliberately much simpler than fmt_bin_disk.c: one staging frame, no ring,
 * no writer thread.  The record pump (logger_task) already decouples this
 * from the state-machine hot path, and fs_write() is where the blocking
 * belongs.  Fewer moving parts is the point -- this backend exists because
 * the elaborate one loses data when the board stops unexpectedly.
 *
 * Copyright (c) 2026 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <aurora/lib/data_logger.h>

/* Provides a no-op stub when CONFIG_AURORA_NOTIFY_DISK_LED is off. */
#include <aurora/lib/disk_led.h>

LOG_MODULE_DECLARE(data_logger, CONFIG_DATA_LOGGER_LOG_LEVEL);

#define BIN_REC_SIZE   ((size_t)sizeof(struct aurora_bin_record))
#define BIN_HDR_SIZE   ((size_t)sizeof(struct aurora_bin_frame_header))
#define BIN_FRAME_SIZE ((size_t)CONFIG_DATA_LOGGER_BIN_FRAME_SIZE)
#define BIN_BUF_ALIGN  CONFIG_DATA_LOGGER_BIN_BUF_ALIGN

BUILD_ASSERT(BIN_FRAME_SIZE >= BIN_HDR_SIZE + BIN_REC_SIZE,
	     "frame must hold at least one header + one record");
BUILD_ASSERT((BIN_FRAME_SIZE - BIN_HDR_SIZE) % BIN_REC_SIZE == 0,
	     "frame payload should be a whole number of records");

/* One frame in flight.  Static rather than per-logger: exactly one binary
 * flight log is open at a time, and the buffer is 4 KiB.
 */
static uint8_t bin_frame[BIN_FRAME_SIZE] __aligned(BIN_BUF_ALIGN);

struct bin_fs_ctx {
	struct fs_file_t file;
	uint64_t flight_id;
	uint32_t next_seq;
	size_t   prod_used;  /**< bytes filled in bin_frame, incl. header */
	bool     file_open;
	int      sticky_err; /**< first write error; latched, reported upward */
};

static struct bin_fs_ctx g_ctx;

/* Path of the file this session writes, as resolved by data_logger_init().
 * The converter has no logger handle to ask, and the file is no longer at a
 * fixed index (see data_logger_formatter::append_mode), so the writer leaves
 * it here.  Empty until the first log of this boot has been opened.
 */
static char g_active_path[DATA_LOGGER_PATH_MAX];

/* -------------------------------------------------------------------------- */
/*  Frame handling                                                            */
/* -------------------------------------------------------------------------- */

static void bin_frame_init(struct bin_fs_ctx *ctx)
{
	/* 0xFF fill so a partially-used frame terminates the same way it does
	 * on erased flash: the reader stops at the first record whose type is
	 * 0xFF.  Keeping that identical across backends is what lets convert.c
	 * stay backend-agnostic.
	 */
	memset(bin_frame, 0xFF, BIN_FRAME_SIZE);

	struct aurora_bin_frame_header *h =
		(struct aurora_bin_frame_header *)bin_frame;

	memcpy(h->magic, AURORA_BIN_FRAME_MAGIC, sizeof(h->magic));
	h->version    = AURORA_BIN_VERSION;
	h->reserved0  = 0;
	h->reserved1  = 0;
	h->seq        = ctx->next_seq++;
	h->flight_id  = ctx->flight_id;
	h->base_ts_ns = k_ticks_to_ns_floor64(k_uptime_ticks());

	ctx->prod_used = BIN_HDR_SIZE;
}

/**
 * @brief Commit the staging frame and start a fresh one.
 *
 * Always writes a whole BIN_FRAME_SIZE, padding included.  The file is
 * therefore always a whole number of frames, which is the invariant
 * convert.c relies on to index frames by multiplication instead of scanning.
 * A partially-filled frame costs tail padding; at the flush cadence that is
 * a few KiB per flight, which is not worth trading the invariant for.
 */
static int bin_commit_frame(struct bin_fs_ctx *ctx)
{
	ssize_t wr;

	if (!ctx->file_open) {
		return -ENOENT;
	}

	wr = fs_write(&ctx->file, bin_frame, BIN_FRAME_SIZE);
	if (wr < 0) {
		if (ctx->sticky_err == 0) {
			ctx->sticky_err = (int)wr;
		}
		LOG_ERR("bin_fs: frame write failed (%d)", (int)wr);
		return (int)wr;
	}
	if ((size_t)wr != BIN_FRAME_SIZE) {
		if (ctx->sticky_err == 0) {
			ctx->sticky_err = -EIO;
		}
		LOG_ERR("bin_fs: short frame write (%d/%zu)",
			(int)wr, BIN_FRAME_SIZE);
		return -EIO;
	}

	disk_led_activity();
	bin_frame_init(ctx);

	return 0;
}

/* -------------------------------------------------------------------------- */
/*  Append recovery                                                           */
/* -------------------------------------------------------------------------- */

/**
 * @brief Pick up where the previous arm cycle left off.
 *
 * Everything inside one boot session appends to the same file rather than
 * starting a new one, so the session stays a single readable stream: a
 * re-arm, and the remainder of a flight resumed after a watchdog or
 * fatal-error reset, both land here.  Which file that is comes from
 * data_logger_init(); an operator-initiated reboot hands over a fresh one and
 * the probe below simply finds it empty.  Two things have to be recovered
 * from what is already on disk:
 *
 *  - @c flight_id, so the file keeps a single value throughout.  convert.c
 *    treats a change of flight_id as the end of the log, so minting a fresh
 *    one per arm would make the converter stop at the first re-arm.
 *  - @c seq, so frame numbering stays monotonic across the seam.
 *
 * A file whose length is not a whole number of frames was cut short by a
 * reset or a power loss mid-write.  The torn tail is truncated away: it
 * cannot be parsed, and leaving it would push every subsequent frame out of
 * alignment and cost the whole appended flight rather than one frame.
 */
static int bin_fs_resume(struct bin_fs_ctx *ctx, const char *path)
{
	struct fs_dirent entry;
	struct aurora_bin_frame_header h;
	off_t whole;
	ssize_t rd;
	int rc;

	rc = fs_stat(path, &entry);
	if (rc != 0 || entry.size < (off_t)BIN_FRAME_SIZE) {
		/* Nothing usable to append to; start a fresh log. */
		return -ENOENT;
	}

	whole = ((off_t)entry.size / (off_t)BIN_FRAME_SIZE) * (off_t)BIN_FRAME_SIZE;

	if (whole != (off_t)entry.size) {
		LOG_WRN("bin_fs: %s ends mid-frame (%u B); truncating to %u B",
			path, (unsigned int)entry.size, (unsigned int)whole);
		rc = fs_truncate(&ctx->file, whole);
		if (rc != 0) {
			LOG_ERR("bin_fs: truncate to frame boundary failed (%d)",
				rc);
			return rc;
		}
	}

	/* The last whole frame carries the highest seq. */
	rc = fs_seek(&ctx->file, whole - (off_t)BIN_FRAME_SIZE, FS_SEEK_SET);
	if (rc != 0) {
		return rc;
	}

	rd = fs_read(&ctx->file, &h, sizeof(h));
	if (rd != (ssize_t)sizeof(h)) {
		return (rd < 0) ? (int)rd : -EIO;
	}

	if (memcmp(h.magic, AURORA_BIN_FRAME_MAGIC, sizeof(h.magic)) != 0 ||
	    h.version != AURORA_BIN_VERSION) {
		LOG_WRN("bin_fs: %s is not an AURORA v%u log; starting fresh",
			path, AURORA_BIN_VERSION);
		return -EINVAL;
	}

	ctx->flight_id = h.flight_id;
	ctx->next_seq  = h.seq + 1U;

	rc = fs_seek(&ctx->file, whole, FS_SEEK_SET);
	if (rc != 0) {
		return rc;
	}

	LOG_INF("bin_fs: appending to %s at frame %u (flight_id=%llu)",
		path, ctx->next_seq, (unsigned long long)ctx->flight_id);

	return 0;
}

/* -------------------------------------------------------------------------- */
/*  Formatter vtable                                                          */
/* -------------------------------------------------------------------------- */

static int bin_fs_init(struct data_logger *logger, const char *path)
{
	struct bin_fs_ctx *ctx = &g_ctx;
	int rc;

	if (ctx->file_open) {
		LOG_ERR("bin_fs: a binary log is already open");
		return -EBUSY;
	}

	memset(ctx, 0, sizeof(*ctx));
	fs_file_t_init(&ctx->file);

	/* Read access is needed for the resume probe below; the write cursor
	 * is positioned explicitly rather than with FS_O_APPEND so the
	 * truncate-and-seek path stays in control of it.
	 */
	rc = fs_open(&ctx->file, path, FS_O_CREATE | FS_O_RDWR);
	if (rc != 0) {
		LOG_ERR("bin_fs: cannot open %s (%d)", path, rc);
		return rc;
	}
	ctx->file_open = true;

	if (bin_fs_resume(ctx, path) != 0) {
		/* Fresh log: seek to the end so a file we could not parse is
		 * appended to rather than overwritten. Losing the ability to
		 * read an old log is better than destroying it.
		 */
		ctx->flight_id = k_ticks_to_ns_floor64(k_uptime_ticks());
		ctx->next_seq  = 0U;

		rc = fs_seek(&ctx->file, 0, FS_SEEK_END);
		if (rc != 0) {
			LOG_ERR("bin_fs: seek to end failed (%d)", rc);
			(void)fs_close(&ctx->file);
			ctx->file_open = false;
			return rc;
		}

		LOG_INF("bin_fs: new flight log %s (flight_id=%llu)", path,
			(unsigned long long)ctx->flight_id);
	}

	bin_frame_init(ctx);
	logger->ctx = ctx;

	strncpy(g_active_path, path, sizeof(g_active_path) - 1);
	g_active_path[sizeof(g_active_path) - 1] = '\0';

	return 0;
}

static int bin_fs_write_header(struct data_logger *logger)
{
	/* The format's header is per-frame, written by bin_frame_init(). */
	ARG_UNUSED(logger);
	return 0;
}

static int bin_fs_write_datapoint(struct data_logger *logger,
				  const struct datapoint *dp)
{
	struct bin_fs_ctx *ctx = logger->ctx;

	if (ctx == NULL) {
		return -EINVAL;
	}
	if (ctx->sticky_err != 0) {
		return ctx->sticky_err;
	}

	if (ctx->prod_used + BIN_REC_SIZE > BIN_FRAME_SIZE) {
		int rc = bin_commit_frame(ctx);

		if (rc != 0) {
			return rc;
		}
	}

	struct aurora_bin_frame_header *h =
		(struct aurora_bin_frame_header *)bin_frame;
	struct aurora_bin_record *rec =
		(struct aurora_bin_record *)(bin_frame + ctx->prod_used);

	uint64_t delta_ns = dp->timestamp_ns >= h->base_ts_ns
		? dp->timestamp_ns - h->base_ts_ns : 0;

	rec->type          = (uint8_t)dp->type;
	rec->channel_count = dp->channel_count;
	rec->reserved      = 0;
	rec->ts_delta_us   = (uint32_t)(delta_ns / 1000U);

	for (int i = 0; i < DP_MAX_CHANNELS; i++) {
		if (i < dp->channel_count) {
			rec->channels[i].val1 = dp->channels[i].val1;
			rec->channels[i].val2 = dp->channels[i].val2;
		} else {
			rec->channels[i].val1 = 0;
			rec->channels[i].val2 = 0;
		}
	}

	ctx->prod_used += BIN_REC_SIZE;

	return 0;
}

static int bin_fs_flush(struct data_logger *logger)
{
	struct bin_fs_ctx *ctx = logger->ctx;
	int rc = 0;

	if (ctx == NULL || !ctx->file_open) {
		return -EINVAL;
	}

	/* Commit whatever the frame holds so a reset after this point cannot
	 * lose it, then push FATFS's own cache out to the card.  Committing a
	 * short frame is what makes the log survive an unexpected stop -- the
	 * whole reason this backend exists -- so the padding is the intended
	 * trade, not waste.
	 */
	if (ctx->prod_used > BIN_HDR_SIZE) {
		rc = bin_commit_frame(ctx);
		if (rc != 0) {
			return rc;
		}
	}

	rc = fs_sync(&ctx->file);
	if (rc != 0) {
		LOG_ERR("bin_fs: sync failed (%d)", rc);
	}

	return rc;
}

static int bin_fs_on_event(struct data_logger *logger,
			   enum data_logger_event ev)
{
	struct bin_fs_ctx *ctx = logger->ctx;

	if (ctx == NULL) {
		return -EINVAL;
	}

	switch (ev) {
	case DLE_BOOST:
		LOG_INF("bin_fs: BOOST at seq=%u", ctx->next_seq);
		break;
	case DLE_LANDED:
		LOG_INF("bin_fs: LANDED at seq=%u", ctx->next_seq);
		break;
	}

	/* Get the pre-boost window on the card immediately: BOOST is the last
	 * moment before the part of the flight most likely to end in a reset.
	 */
	return bin_fs_flush(logger);
}

static int bin_fs_close(struct data_logger *logger)
{
	struct bin_fs_ctx *ctx = logger->ctx;
	int rc;

	if (ctx == NULL) {
		return -EINVAL;
	}

	(void)bin_fs_flush(logger);

	rc = ctx->file_open ? fs_close(&ctx->file) : 0;
	ctx->file_open = false;
	logger->ctx = NULL;

	if (rc != 0) {
		LOG_ERR("bin_fs: close failed (%d)", rc);
	}

	return rc;
}

const struct data_logger_formatter data_logger_bin_formatter = {
	.init            = bin_fs_init,
	.write_header    = bin_fs_write_header,
	.write_datapoint = bin_fs_write_datapoint,
	.flush           = bin_fs_flush,
	.close           = bin_fs_close,
	.on_event        = bin_fs_on_event,
	/* Re-arming continues the same file; see bin_fs_resume(). */
	.append_mode     = true,
	.file_ext        = "bin",
	.name            = "bin",
};

/* -------------------------------------------------------------------------- */
/*  Converter IO                                                              */
/* -------------------------------------------------------------------------- */

#if defined(CONFIG_DATA_LOGGER_CONVERT)

/* Opened independently of the writer: conversion only runs once the live
 * logger has been closed, so there is no concurrent access to serialise.
 */
static struct fs_file_t g_io_file;
static bool g_io_open;
static size_t g_io_size;

int bin_io_open(void)
{
	struct fs_dirent entry;
	const char *path;
	int rc;

	if (g_io_open) {
		return 0;
	}

	/* Whatever the writer actually opened this session.  The Kconfig path
	 * only covers the case where nothing has been logged since boot --
	 * converting a log left behind by a previous run.
	 */
	path = (g_active_path[0] != '\0') ? g_active_path
					  : CONFIG_DATA_LOGGER_BIN_FS_PATH;

	rc = fs_stat(path, &entry);
	if (rc != 0) {
		LOG_ERR("bin_fs: no flight log at %s (%d)", path, rc);
		return rc;
	}

	fs_file_t_init(&g_io_file);
	rc = fs_open(&g_io_file, path, FS_O_READ);
	if (rc != 0) {
		return rc;
	}

	/* Round down: a torn tail is not a readable frame. */
	g_io_size = ((size_t)entry.size / BIN_FRAME_SIZE) * BIN_FRAME_SIZE;
	g_io_open = true;

	return 0;
}

int bin_io_close(void)
{
	int rc = g_io_open ? fs_close(&g_io_file) : 0;

	g_io_open = false;
	g_io_size = 0;

	return rc;
}

int bin_io_read(off_t off, void *buf, size_t len)
{
	ssize_t rd;
	int rc;

	if (!g_io_open) {
		return -ENOENT;
	}

	rc = fs_seek(&g_io_file, off, FS_SEEK_SET);
	if (rc != 0) {
		return rc;
	}

	rd = fs_read(&g_io_file, buf, len);
	if (rd < 0) {
		return (int)rd;
	}
	if ((size_t)rd != len) {
		return -EIO;
	}

	return 0;
}

size_t bin_io_total_size(void)
{
	return g_io_size;
}

int bin_io_window_start_hint(off_t *out_offset, uint32_t *out_seq,
			     uint64_t *out_flight_id)
{
	struct aurora_bin_frame_header h;
	int rc;

	if (!g_io_open || g_io_size < BIN_FRAME_SIZE) {
		return -ENOENT;
	}

	/* Purely linear and append-only, so frame 0 is always the start of the
	 * captured window -- no scan needed.
	 */
	rc = bin_io_read(0, &h, sizeof(h));
	if (rc != 0) {
		return rc;
	}

	if (memcmp(h.magic, AURORA_BIN_FRAME_MAGIC, sizeof(h.magic)) != 0 ||
	    h.version != AURORA_BIN_VERSION) {
		return -ENOENT;
	}

	*out_offset    = 0;
	*out_seq       = h.seq;
	*out_flight_id = h.flight_id;

	return 0;
}

#endif /* CONFIG_DATA_LOGGER_CONVERT */
