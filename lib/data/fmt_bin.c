/**
 * @file fmt_bin.c
 * @brief Raw-flash binary formatter for the live flight log.
 *
 * Writes a frame-aligned binary log directly to a fixed flash partition —
 * no filesystem in the live path. The partition is selected via the device
 * tree chosen entry @c auxspace,flight-log:
 *
 * @code{.dts}
 * &flash0 {
 *     partitions {
 *         compatible = "fixed-partitions";
 *         #address-cells = <1>;
 *         #size-cells  = <1>;
 *         flight_log: partition@300000 {
 *             label = "flight_log";
 *             reg   = <0x300000 DT_SIZE_M(1)>;
 *         };
 *     };
 * };
 * / { chosen { auxspace,flight-log = &flight_log; }; };
 * @endcode
 *
 * On-disk layout (see data_logger.h for the structs):
 *
 *   offset 0          [frame_header][record][record]...[record][0xFF padding]
 *   offset frame_size [frame_header][record]...
 *   ...
 *
 * The partition is treated as a circular ring. Frames are written
 * sequentially; when the writer hits the partition end it wraps back to
 * offset 0, overwriting the oldest pre-boost frame. On
 * @ref data_logger_event(DLE_BOOST) the formatter captures the seq
 * number of the next-to-be-written frame and from then on caps the
 * writer at @c ring_frames post-boost commits — the cap point is
 * exactly where the next write would clobber the BOOST frame, so the
 * BOOST frame and everything between BOOST and the cap are preserved.
 *
 * The post-landed pad is implemented by the upstream caller scheduling
 * @c data_logger_close some milliseconds after DLE_LANDED; the writer
 * itself doesn't know about LANDED beyond a log line.
 *
 * The converter walks frames by seq number rather than by physical
 * offset; the lowest seq still on flash is the start of the captured
 * window, the highest seq is the end. See convert.c.
 *
 * Producer side (called under the data_logger mutex by the upstream logger
 * thread) memcpys each record straight into a DMA-aligned RAM staging
 * buffer. When a buffer fills, the producer hands its index to a writer
 * thread via a small free/flush index ring and immediately picks up a fresh
 * buffer with a new frame header. The writer thread issues one
 * @c flash_area_erase + @c flash_area_write per frame — so 1 kHz × ~5
 * records collapses from ~5000 tiny fs_writes/s into ~50 page-aligned
 * raw-flash writes/s, with no FATFS / littlefs overhead in the path.
 *
 * Records preserve the @c sensor_value channels losslessly (val1+val2),
 * so post-flight conversion can replay filters and the state machine
 * bit-exactly.
 *
 * Only one bin logger instance can be live at a time; @ref bin_init
 * rejects a second concurrent open.
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

#include "bin_io.h"

LOG_MODULE_DECLARE(data_logger, CONFIG_DATA_LOGGER_LOG_LEVEL);

/* -------------------------------------------------------------------------- */
/*  DT partition lookup                                                       */
/* -------------------------------------------------------------------------- */

#if !DT_HAS_CHOSEN(auxspace_flight_log)
#error "DATA_LOGGER_BIN requires DT chosen 'auxspace,flight-log' to point at a fixed-partition node. See aurora/lib/data/fmt_bin.c file header for an example overlay."
#endif

#define BIN_FLASH_PARTITION  DT_CHOSEN(auxspace_flight_log)
#define BIN_FLASH_AREA_ID    DT_FIXED_PARTITION_ID(BIN_FLASH_PARTITION)
#define BIN_FLASH_AREA_SIZE  DT_REG_SIZE(BIN_FLASH_PARTITION)

/* -------------------------------------------------------------------------- */
/*  Frame & buffer geometry                                                   */
/* -------------------------------------------------------------------------- */

#define BIN_REC_SIZE   ((size_t)sizeof(struct aurora_bin_record))
#define BIN_HDR_SIZE   ((size_t)sizeof(struct aurora_bin_frame_header))
#define BIN_FRAME_SIZE ((size_t)CONFIG_DATA_LOGGER_BIN_FRAME_SIZE)
#define BIN_BUF_COUNT  CONFIG_DATA_LOGGER_BIN_BUF_COUNT
#define BIN_BUF_ALIGN  CONFIG_DATA_LOGGER_BIN_BUF_ALIGN

BUILD_ASSERT(BIN_FRAME_SIZE >= BIN_HDR_SIZE + BIN_REC_SIZE,
	     "frame must hold at least one header + one record");
BUILD_ASSERT((BIN_FRAME_SIZE - BIN_HDR_SIZE) % BIN_REC_SIZE == 0,
	     "frame payload should be a whole number of records");
BUILD_ASSERT(BIN_BUF_COUNT >= 2, "BIN needs at least double-buffering");
BUILD_ASSERT(BIN_FLASH_AREA_SIZE % BIN_FRAME_SIZE == 0,
	     "flight_log partition size must be a multiple of the frame size");

struct bin_buf {
	uint8_t data[BIN_FRAME_SIZE];
	size_t  used;
};

/* DMA-aligned static pool. Single live bin formatter is supported. */
static struct bin_buf bin_bufs[BIN_BUF_COUNT] __aligned(BIN_BUF_ALIGN);

struct bin_ctx {
	const struct flash_area *fa;
	uint64_t flight_id;
	uint32_t next_seq;        /* producer side */
	off_t    write_offset;    /* writer side: next frame offset */
	uint32_t ring_frames;     /* partition size in whole frames */
	int      active_idx;      /* producer's currently-held buffer */
	atomic_t sticky_err;      /* first error observed (writer or producer) */

	/* Boost-freeze gate. Holds UINT32_MAX while we're in pre-boost
	 * circular mode; switches to the boost_seq value (the seq of the
	 * first post-boost frame) when on_event(DLE_BOOST) fires. The
	 * writer reads it to decide whether the cap has been reached.
	 */
	atomic_t boost_seq_or_max;
};

#define BIN_BOOST_NOT_SEEN ((atomic_val_t)UINT32_MAX)

/* Single live instance — see file comment. */
static struct bin_ctx g_bin_ctx;
static atomic_t g_bin_open = ATOMIC_INIT(0);

/* Free pool: indices of buffers ready to be filled by the producer.
 * Capacity = BIN_BUF_COUNT so every buffer can sit here at once.
 */
K_MSGQ_DEFINE(bin_free_q, sizeof(int), BIN_BUF_COUNT, 4);

/* Submit pool: indices the writer should write. Capacity = BIN_BUF_COUNT
 * for buffers, plus one extra slot for the drain sentinel value (-1).
 */
K_MSGQ_DEFINE(bin_flush_q, sizeof(int), BIN_BUF_COUNT + 1, 4);

/* Writer signals this when it processes a drain sentinel; by FIFO ordering
 * every buffer submitted before the sentinel has already been written.
 */
K_SEM_DEFINE(bin_drain_sem, 0, 1);

/* -------------------------------------------------------------------------- */
/*  Writer thread                                                             */
/* -------------------------------------------------------------------------- */

static void bin_writer_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	int idx;

	for (;;) {
		if (k_msgq_get(&bin_flush_q, &idx, K_FOREVER) != 0) {
			continue;
		}

		if (idx < 0) {
			k_sem_give(&bin_drain_sem);
			continue;
		}

		struct bin_buf *b = &bin_bufs[idx];

		if (atomic_get(&g_bin_open) && b->used > 0) {
			const struct aurora_bin_frame_header *bh =
				(const struct aurora_bin_frame_header *)b->data;
			uint32_t buf_seq = bh->seq;
			atomic_val_t boost = atomic_get(
					&g_bin_ctx.boost_seq_or_max);

			/* Post-boost cap: once we've written ring_frames frames
			 * with seq >= boost_seq, the next slot would clobber
			 * the BOOST frame itself. Drop everything past that
			 * point so the BOOST frame and the post-boost capture
			 * remain intact.
			 *
			 * Frames with seq < boost_seq are pre-boost in-flight
			 * buffers (the active buffer at the moment of BOOST
			 * still carries a pre-boost seq); they are written
			 * normally at write_offset so the boundary lands
			 * cleanly on flash.
			 */
			bool capped = (boost != BIN_BOOST_NOT_SEEN) &&
				      (buf_seq >= (uint32_t)boost) &&
				      ((buf_seq - (uint32_t)boost) >=
				       g_bin_ctx.ring_frames);

			if (capped) {
				/* Silent drop — we keep the post-boost slice
				 * already on flash exactly as recorded.
				 */
			} else {
				off_t off = g_bin_ctx.write_offset;

				/* Circular wrap at partition end. Pre-boost
				 * data here is the oldest in the ring and is
				 * the one we'd sacrifice anyway.
				 */
				if ((size_t)off + BIN_FRAME_SIZE >
				    (size_t)BIN_FLASH_AREA_SIZE) {
					off = 0;
					g_bin_ctx.write_offset = 0;
				}

				int rc = flash_area_erase(g_bin_ctx.fa, off,
							  BIN_FRAME_SIZE);
				if (rc == 0) {
					rc = flash_area_write(g_bin_ctx.fa, off,
							      b->data, b->used);
				}

				if (rc != 0) {
					(void)atomic_cas(&g_bin_ctx.sticky_err, 0,
							 (atomic_val_t)rc);
					LOG_ERR("bin: flash write at %ld "
						"failed (%d)", (long)off, rc);
				} else {
					g_bin_ctx.write_offset =
						off + BIN_FRAME_SIZE;
				}
			}
		}

		b->used = 0;
		(void)k_msgq_put(&bin_free_q, &idx, K_NO_WAIT);
	}
}

K_THREAD_DEFINE(bin_writer_th, CONFIG_DATA_LOGGER_BIN_WRITER_STACK_SIZE,
		bin_writer_fn, NULL, NULL, NULL,
		CONFIG_DATA_LOGGER_BIN_WRITER_PRIO, 0, 0);

/* -------------------------------------------------------------------------- */
/*  Producer-side helpers                                                     */
/* -------------------------------------------------------------------------- */

/* Initialize a fresh frame at the start of a buffer. */
static void bin_frame_init(struct bin_ctx *ctx, struct bin_buf *b)
{
	/* Clear so any unwritten record slot reads as type=0xFF (matches
	 * the post-erase flash state and the converter's stop condition).
	 */
	memset(b->data, 0xFF, sizeof(b->data));

	struct aurora_bin_frame_header *h =
		(struct aurora_bin_frame_header *)b->data;

	memcpy(h->magic, AURORA_BIN_FRAME_MAGIC, sizeof(h->magic));
	h->version     = AURORA_BIN_VERSION;
	h->reserved0   = 0;
	h->reserved1   = 0;
	h->seq         = ctx->next_seq++;
	h->flight_id   = ctx->flight_id;
	h->base_ts_ns  = k_ticks_to_ns_floor64(k_uptime_ticks());

	b->used = BIN_HDR_SIZE;
}

/* Take the next free buffer for the producer and start a new frame in it. */
static int bin_take_free(struct bin_ctx *ctx, k_timeout_t to)
{
	int idx;
	int rc = k_msgq_get(&bin_free_q, &idx, to);

	if (rc != 0) {
		return rc;
	}

	ctx->active_idx = idx;
	bin_frame_init(ctx, &bin_bufs[idx]);
	return 0;
}

/* Submit the active buffer (always non-empty, contains at least the header)
 * to the writer and pick up a fresh one.
 */
static int bin_rotate(struct bin_ctx *ctx)
{
	int idx = ctx->active_idx;

	if (k_msgq_put(&bin_flush_q, &idx, K_NO_WAIT) != 0) {
		/* flush_q only fills if the writer is wedged. */
		bin_bufs[idx].used = 0;
		(void)atomic_cas(&g_bin_ctx.sticky_err, 0,
				 (atomic_val_t)-EBUSY);
		return -EBUSY;
	}

	return bin_take_free(ctx,
		K_MSEC(CONFIG_DATA_LOGGER_BIN_PRODUCER_TIMEOUT_MS));
}

/* Inject a sentinel and wait for the writer to acknowledge it. After this
 * returns, every buffer submitted to flush_q before the sentinel has been
 * committed to flash (modulo flash errors, which become sticky).
 */
static int bin_drain_writer(void)
{
	int sentinel = -1;

	k_sem_reset(&bin_drain_sem);

	if (k_msgq_put(&bin_flush_q, &sentinel,
		       K_MSEC(CONFIG_DATA_LOGGER_BIN_PRODUCER_TIMEOUT_MS)) != 0) {
		return -EBUSY;
	}

	if (k_sem_take(&bin_drain_sem,
		       K_MSEC(CONFIG_DATA_LOGGER_BIN_FLUSH_TIMEOUT_MS)) != 0) {
		return -ETIMEDOUT;
	}

	return 0;
}

/* -------------------------------------------------------------------------- */
/*  Formatter vtable                                                          */
/* -------------------------------------------------------------------------- */

/* The bin formatter ignores @p path — its destination is the flash
 * partition selected via DT chosen. The path argument is preserved by the
 * core only to satisfy the formatter signature.
 */
static int bin_init(struct data_logger *logger, const char *path)
{
	ARG_UNUSED(path);

	if (!atomic_cas(&g_bin_open, 0, 1)) {
		LOG_ERR("bin: a binary logger is already open");
		return -EBUSY;
	}

	struct bin_ctx *ctx = &g_bin_ctx;
	int idx;
	int rc;

	memset(ctx, 0, sizeof(*ctx));
	atomic_set(&ctx->sticky_err, 0);

	rc = flash_area_open(BIN_FLASH_AREA_ID, &ctx->fa);
	if (rc != 0) {
		LOG_ERR("bin: flash_area_open(id=%u) failed (%d)",
			(unsigned)BIN_FLASH_AREA_ID, rc);
		atomic_set(&g_bin_open, 0);
		return rc;
	}

	/* Use the high-resolution monotonic clock for a per-flight ID; two
	 * concurrent flights would need to share a boot, which can't happen.
	 */
	ctx->flight_id    = k_ticks_to_ns_floor64(k_uptime_ticks());
	ctx->next_seq     = 0;
	ctx->write_offset = 0;
	ctx->ring_frames  = (uint32_t)(BIN_FLASH_AREA_SIZE / BIN_FRAME_SIZE);
	ctx->active_idx   = -1;
	atomic_set(&ctx->boost_seq_or_max, BIN_BOOST_NOT_SEEN);

	/* Drop any leftovers from a prior aborted session and re-prime the
	 * free pool with every buffer.
	 */
	while (k_msgq_get(&bin_flush_q, &idx, K_NO_WAIT) == 0) { }
	while (k_msgq_get(&bin_free_q,  &idx, K_NO_WAIT) == 0) { }
	for (int i = 0; i < BIN_BUF_COUNT; i++) {
		bin_bufs[i].used = 0;
		(void)k_msgq_put(&bin_free_q, &i, K_NO_WAIT);
	}

	rc = bin_take_free(ctx, K_NO_WAIT);
	if (rc != 0) {
		flash_area_close(ctx->fa);
		ctx->fa = NULL;
		atomic_set(&g_bin_open, 0);
		return rc;
	}

	logger->ctx = ctx;
	return 0;
}

/* The legacy "file header" hook is a no-op now — every frame on flash
 * carries its own header, so there's no separate one-shot preamble to
 * write. The flush that data_logger_init issues immediately after this
 * still drains the (empty-of-records) frame; that's harmless.
 */
static int bin_write_header(struct data_logger *logger)
{
	ARG_UNUSED(logger);
	return 0;
}

static int bin_write_datapoint(struct data_logger *logger,
			       const struct datapoint *dp)
{
	struct bin_ctx *ctx = logger->ctx;
	int err = (int)atomic_get(&ctx->sticky_err);

	if (err != 0) {
		return err;
	}

	struct bin_buf *b = &bin_bufs[ctx->active_idx];

	if (b->used + BIN_REC_SIZE > BIN_FRAME_SIZE) {
		int rc = bin_rotate(ctx);

		if (rc != 0) {
			return rc;
		}
		b = &bin_bufs[ctx->active_idx];
	}

	struct aurora_bin_frame_header *h =
		(struct aurora_bin_frame_header *)b->data;
	struct aurora_bin_record *rec =
		(struct aurora_bin_record *)(b->data + b->used);

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

	b->used += BIN_REC_SIZE;
	return 0;
}

static int bin_flush(struct data_logger *logger)
{
	struct bin_ctx *ctx = logger->ctx;
	int rc;

	/* Push the current frame (header + however many records so far) and
	 * pick up a fresh one. The partial frame's tail stays 0xFF so the
	 * converter stops at the first unwritten record slot.
	 */
	if (bin_bufs[ctx->active_idx].used > BIN_HDR_SIZE) {
		rc = bin_rotate(ctx);
		if (rc != 0) {
			LOG_ERR("bin_flush: rotate failed (%d)", rc);
			return rc;
		}
	}

	rc = bin_drain_writer();
	if (rc != 0) {
		LOG_ERR("bin_flush: drain failed (%d)", rc);
		return rc;
	}

	int err = (int)atomic_get(&ctx->sticky_err);

	return err;
}

/* DLE_BOOST: capture the seq number of the next-to-be-written frame.
 * From here on the writer thread treats the partition as linear-with-cap
 * — it keeps wrapping forward until ring_frames post-boost frames have
 * been committed, then drops everything else so the BOOST frame and the
 * subsequent flight slice survive untouched.
 *
 * DLE_LANDED is informational; the upstream caller schedules close some
 * time later so the post-landed pad gets captured naturally as the tail
 * of the post-boost ring.
 *
 * Called under the logger mutex (see data_logger_event), so producer-
 * side fields can be touched directly.
 */
static int bin_on_event(struct data_logger *logger, enum data_logger_event ev)
{
	struct bin_ctx *ctx = logger->ctx;

	if (ctx == NULL) {
		return -EINVAL;
	}

	switch (ev) {
	case DLE_BOOST:
		(void)atomic_cas(&ctx->boost_seq_or_max, BIN_BOOST_NOT_SEEN,
				 (atomic_val_t)ctx->next_seq);
		LOG_INF("bin: BOOST captured at seq=%u", ctx->next_seq);
		break;
	case DLE_LANDED:
		LOG_INF("bin: LANDED at seq=%u", ctx->next_seq);
		break;
	}

	return 0;
}

static int bin_close(struct data_logger *logger)
{
	struct bin_ctx *ctx = logger->ctx;

	/* Drain whatever's still buffered. Keep going on flush errors so we
	 * still close the partition and release the active buffer.
	 */
	(void)bin_flush(logger);

	if (ctx->fa != NULL) {
		flash_area_close(ctx->fa);
		ctx->fa = NULL;
	}

	if (ctx->active_idx >= 0) {
		int idx = ctx->active_idx;

		(void)k_msgq_put(&bin_free_q, &idx, K_NO_WAIT);
		ctx->active_idx = -1;
	}

	logger->ctx = NULL;
	atomic_set(&g_bin_open, 0);
	return 0;
}

const struct data_logger_formatter data_logger_bin_formatter = {
	.init            = bin_init,
	.write_header    = bin_write_header,
	.write_datapoint = bin_write_datapoint,
	.flush           = bin_flush,
	.close           = bin_close,
	.on_event        = bin_on_event,
	.file_ext        = "bin",
	.name            = "bin",
};

/* -------------------------------------------------------------------------- */
/*  Converter IO (separate flash_area handle so the converter can run after  */
/*  the writer has closed its own).                                          */
/* -------------------------------------------------------------------------- */

static const struct flash_area *g_convert_fa;

int bin_io_open(void)
{
	int rc = flash_area_open(BIN_FLASH_AREA_ID, &g_convert_fa);

	if (rc != 0) {
		LOG_ERR("bin_io: flash_area_open failed (%d)", rc);
	}
	return rc;
}

int bin_io_close(void)
{
	if (g_convert_fa != NULL) {
		flash_area_close(g_convert_fa);
		g_convert_fa = NULL;
	}
	return 0;
}

int bin_io_read(off_t off, void *buf, size_t len)
{
	if (g_convert_fa == NULL) {
		return -ENODEV;
	}
	return flash_area_read(g_convert_fa, off, buf, len);
}

size_t bin_io_total_size(void)
{
	return (size_t)BIN_FLASH_AREA_SIZE;
}
