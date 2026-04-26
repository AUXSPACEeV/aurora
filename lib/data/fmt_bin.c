/**
 * @file fmt_bin.c
 * @brief Fixed-size binary formatter backend with N-buffered async writes.
 *
 * Writes a 16-byte file header followed by one 40-byte record per datapoint.
 * The on-disk layout is unchanged from the original single-write design;
 * what changed is how the bytes get there.
 *
 * Producer side (called under the data_logger mutex by the upstream logger
 * thread) memcpys the record straight into a DMA-aligned RAM staging buffer.
 * When the active buffer fills, the producer hands its index to a writer
 * thread via a small free/flush index ring and immediately picks up a fresh
 * buffer. A dedicated writer thread drains the flush ring with one large
 * `fs_write(buf, BIN_BUF_SIZE)` per buffer — so 1 kHz × ~5 records (40 B
 * each) collapses from ~5000 tiny fs_write calls/s into ~50 sector-aligned
 * ones, which is what FATFS/littlefs on SD/NOR actually want.
 *
 * The staging buffers are statically allocated with a stricter alignment
 * (`CONFIG_DATA_LOGGER_BIN_BUF_ALIGN`, default 32 B) so the underlying
 * block driver can DMA into them without bounce-buffering.
 *
 * Multi-byte fields are stored in native little-endian layout; every
 * supported target (RP2040, RP2350, ESP32-S3, qemu_x86) is LE.
 *
 * Only one bin logger instance can be live at a time — `bin_init` rejects a
 * second concurrent open. The formatter is intended for the live flight log;
 * conversion targets (CSV, InfluxDB) use their own formatters.
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

#define BIN_REC_SIZE    ((size_t)sizeof(struct aurora_bin_record))
#define BIN_HDR_SIZE    ((size_t)sizeof(struct aurora_bin_header))
#define BIN_BUF_SIZE    ((size_t)CONFIG_DATA_LOGGER_BIN_BUF_SIZE)
#define BIN_BUF_COUNT   CONFIG_DATA_LOGGER_BIN_BUF_COUNT
#define BIN_BUF_ALIGN   CONFIG_DATA_LOGGER_BIN_BUF_ALIGN

BUILD_ASSERT(BIN_BUF_SIZE >= BIN_REC_SIZE + BIN_HDR_SIZE,
	     "DATA_LOGGER_BIN_BUF_SIZE must hold the header plus one record");
BUILD_ASSERT(BIN_BUF_COUNT >= 2, "BIN needs at least double-buffering");

/* Cache-line / DMA-aligned staging buffers. The data array is the only
 * thing the block driver touches, so it gets the alignment.
 */
struct bin_buf {
	uint8_t data[BIN_BUF_SIZE];
	size_t  used;
};

static struct bin_buf bin_bufs[BIN_BUF_COUNT] __aligned(BIN_BUF_ALIGN);

struct bin_ctx {
	struct fs_file_t file;
	int active_idx;       /* buffer currently held by producer */
	atomic_t sticky_err;  /* first FS error observed by writer  */
};

/* Single live instance — see file comment. */
static struct bin_ctx g_bin_ctx;
static atomic_t g_bin_open = ATOMIC_INIT(0);

/* Free pool: indices of buffers ready to be filled by the producer.
 * Capacity = BIN_BUF_COUNT so every buffer can sit here at once.
 */
K_MSGQ_DEFINE(bin_free_q, sizeof(int), BIN_BUF_COUNT, 4);

/* Submit pool: indices the writer should fs_write. Capacity = BIN_BUF_COUNT
 * for buffers, plus one extra slot for the drain sentinel value (-1).
 */
K_MSGQ_DEFINE(bin_flush_q, sizeof(int), BIN_BUF_COUNT + 1, 4);

/* Writer signals this when it processes a drain sentinel; by FIFO ordering
 * every buffer submitted before the sentinel has already been written.
 */
K_SEM_DEFINE(bin_drain_sem, 0, 1);

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
			ssize_t wr = fs_write(&g_bin_ctx.file, b->data, b->used);

			if (wr < 0) {
				(void)atomic_cas(&g_bin_ctx.sticky_err, 0,
						 (atomic_val_t)wr);
				LOG_ERR("bin: fs_write failed (%zd)", wr);
			} else if ((size_t)wr != b->used) {
				(void)atomic_cas(&g_bin_ctx.sticky_err, 0,
						 (atomic_val_t)-EIO);
				LOG_ERR("bin: short write %zd/%zu",
					wr, b->used);
			}
		}

		b->used = 0;
		(void)k_msgq_put(&bin_free_q, &idx, K_NO_WAIT);
	}
}

K_THREAD_DEFINE(bin_writer_th, CONFIG_DATA_LOGGER_BIN_WRITER_STACK_SIZE,
		bin_writer_fn, NULL, NULL, NULL,
		CONFIG_DATA_LOGGER_BIN_WRITER_PRIO, 0, 0);

/* Take the next free buffer for the producer. */
static int bin_take_free(struct bin_ctx *ctx, k_timeout_t to)
{
	int idx;
	int rc = k_msgq_get(&bin_free_q, &idx, to);

	if (rc != 0) {
		return rc;
	}

	bin_bufs[idx].used = 0;
	ctx->active_idx = idx;
	return 0;
}

/* Submit the active buffer (if non-empty) and pick up a fresh one. */
static int bin_rotate(struct bin_ctx *ctx)
{
	int idx = ctx->active_idx;

	if (bin_bufs[idx].used > 0) {
		if (k_msgq_put(&bin_flush_q, &idx, K_NO_WAIT) != 0) {
			/* flush_q only fills if the writer is wedged. Drop
			 * this buffer's contents so we don't lose forward
			 * progress, but report so the upstream knows.
			 */
			bin_bufs[idx].used = 0;
			(void)atomic_cas(&g_bin_ctx.sticky_err, 0,
					 (atomic_val_t)-EBUSY);
			return -EBUSY;
		}
	} else {
		/* Empty buffer — return it directly without bothering the
		 * writer. K_NO_WAIT is safe: free_q has BIN_BUF_COUNT slots.
		 */
		(void)k_msgq_put(&bin_free_q, &idx, K_NO_WAIT);
	}

	return bin_take_free(ctx,
		K_MSEC(CONFIG_DATA_LOGGER_BIN_PRODUCER_TIMEOUT_MS));
}

/* Inject a sentinel and wait for the writer to acknowledge it. After this
 * returns, every buffer submitted to flush_q before the sentinel has been
 * written to the underlying file (modulo FS errors).
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

static int bin_init(struct data_logger *logger, const char *path)
{
	if (!atomic_cas(&g_bin_open, 0, 1)) {
		LOG_ERR("bin: a binary logger is already open");
		return -EBUSY;
	}

	struct bin_ctx *ctx = &g_bin_ctx;
	int idx;
	int rc;

	memset(ctx, 0, sizeof(*ctx));
	atomic_set(&ctx->sticky_err, 0);
	fs_file_t_init(&ctx->file);

	/* Drop any leftovers from a prior aborted session and re-prime the
	 * free pool with every buffer.
	 */
	while (k_msgq_get(&bin_flush_q, &idx, K_NO_WAIT) == 0) { }
	while (k_msgq_get(&bin_free_q,  &idx, K_NO_WAIT) == 0) { }
	for (int i = 0; i < BIN_BUF_COUNT; i++) {
		bin_bufs[i].used = 0;
		(void)k_msgq_put(&bin_free_q, &i, K_NO_WAIT);
	}

	rc = fs_open(&ctx->file, path,
		     FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (rc != 0) {
		LOG_ERR("failed to open %s (%d)", path, rc);
		atomic_set(&g_bin_open, 0);
		return rc;
	}

	rc = bin_take_free(ctx, K_NO_WAIT);
	if (rc != 0) {
		(void)fs_close(&ctx->file);
		atomic_set(&g_bin_open, 0);
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

	struct bin_buf *b = &bin_bufs[ctx->active_idx];

	/* The header is written before any datapoint, so the active buffer
	 * is fresh and definitely has room. The size check is a guard.
	 */
	if (b->used + sizeof(hdr) > BIN_BUF_SIZE) {
		int rc = bin_rotate(ctx);

		if (rc != 0) {
			return rc;
		}
		b = &bin_bufs[ctx->active_idx];
	}

	memcpy(b->data + b->used, &hdr, sizeof(hdr));
	b->used += sizeof(hdr);
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

	if (b->used + BIN_REC_SIZE > BIN_BUF_SIZE) {
		int rc = bin_rotate(ctx);

		if (rc != 0) {
			return rc;
		}
		b = &bin_bufs[ctx->active_idx];
	}

	/* Build the record straight into the staging buffer to skip a copy.
	 * struct aurora_bin_record is __packed so a byte-offset cast is fine.
	 */
	struct aurora_bin_record *rec =
		(struct aurora_bin_record *)(b->data + b->used);

	rec->timestamp_ns  = dp->timestamp_ns;
	rec->type          = (uint8_t)dp->type;
	rec->channel_count = dp->channel_count;
	memset(rec->_pad, 0, sizeof(rec->_pad));

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

	if (bin_bufs[ctx->active_idx].used > 0) {
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

	if (err != 0) {
		return err;
	}

	return fs_sync(&ctx->file);
}

static int bin_close(struct data_logger *logger)
{
	struct bin_ctx *ctx = logger->ctx;

	/* Drain whatever's still buffered. Keep going on flush errors so we
	 * still close the file and release the active buffer.
	 */
	(void)bin_flush(logger);

	int rc = fs_close(&ctx->file);

	int idx = ctx->active_idx;

	(void)k_msgq_put(&bin_free_q, &idx, K_NO_WAIT);

	logger->ctx = NULL;
	atomic_set(&g_bin_open, 0);
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
