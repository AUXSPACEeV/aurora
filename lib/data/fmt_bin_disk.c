/**
 * @file fmt_bin_disk.c
 * @brief Disk-backed binary formatter for the live flight log.
 *
 * Companion to fmt_bin.c.  Where fmt_bin targets a small fixed flash
 * partition and treats it as a circular ring with boost-freeze, this
 * file targets a Zephyr disk-access block device (typically an SD card)
 * with much larger capacity.  The writer is therefore purely linear
 * from the configured sector offset; circular wrap is not used and
 * BOOST events are recorded only as informational log lines.
 *
 * Geometry comes from a DT chosen entry @c auxspace,flight-log-disk
 * pointing at a node of compatible @c auxspaceev,flight-log-disk:
 *
 * @code{.dts}
 * / {
 *     chosen {
 *         auxspace,flight-log-disk = &flight_log_disk;
 *     };
 *
 *     flight_log_disk: flight-log-disk {
 *         compatible = "auxspaceev,flight-log-disk";
 *         disk-name = "MMC";
 *         offset-sectors = <2097152>;
 *         size-sectors   = <1048576>;
 *     };
 * };
 * @endcode
 *
 * The application is responsible for ensuring the configured sector
 * range does not overlap any filesystem mounted on the same disk.
 *
 * The writer thread submits one @c disk_access_write of
 * @c BIN_FRAME_SIZE / sector_size sectors per frame.  No erase is
 * needed; SD cards translate writes through their FTL and unwritten
 * regions read as the manufacturer's default fill.  The frame layout
 * (header + records) is identical to the flash backend, so the
 * converter walks both with the same algorithm.
 *
 * Copyright (c) 2026 Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/drivers/disk.h>
#include <zephyr/logging/log.h>

#include <aurora/lib/data_logger.h>

#include "bin_io.h"

LOG_MODULE_DECLARE(data_logger, CONFIG_DATA_LOGGER_LOG_LEVEL);

/* -------------------------------------------------------------------------- */
/*  DT geometry                                                               */
/* -------------------------------------------------------------------------- */

#if !DT_HAS_CHOSEN(auxspace_flight_log_disk)
#error "DATA_LOGGER_BIN_BACKEND_DISK requires DT chosen 'auxspace,flight-log-disk'. See aurora/lib/data/fmt_bin_disk.c file header for an example overlay."
#endif

#define BIN_DISK_NODE        DT_CHOSEN(auxspace_flight_log_disk)
#define BIN_DISK_NAME        DT_PROP(BIN_DISK_NODE, disk_name)
#define BIN_DISK_OFFSET_SEC  DT_PROP(BIN_DISK_NODE, offset_sectors)
#define BIN_DISK_SIZE_SEC    DT_PROP(BIN_DISK_NODE, size_sectors)

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

struct bin_buf {
	uint8_t data[BIN_FRAME_SIZE];
	size_t  used;
};

static struct bin_buf bin_bufs[BIN_BUF_COUNT] __aligned(BIN_BUF_ALIGN);

struct bin_disk_ctx {
	uint64_t flight_id;
	uint32_t next_seq;          /* producer side */
	uint32_t cur_sector_offset; /* writer side: sectors past disk offset */
	uint32_t sector_size;       /* queried at init from DISK_IOCTL */
	uint32_t sectors_per_frame; /* BIN_FRAME_SIZE / sector_size */
	int      active_idx;
	atomic_t sticky_err;
};

static struct bin_disk_ctx g_bin_ctx;
static atomic_t g_bin_open = ATOMIC_INIT(0);

K_MSGQ_DEFINE(bin_free_q, sizeof(int), BIN_BUF_COUNT, 4);
K_MSGQ_DEFINE(bin_flush_q, sizeof(int), BIN_BUF_COUNT + 1, 4);
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
			uint32_t sec = g_bin_ctx.cur_sector_offset;

			if (sec + g_bin_ctx.sectors_per_frame >
			    (uint32_t)BIN_DISK_SIZE_SEC) {
				/* Linear region exhausted. Stop quietly so
				 * the producer's sticky_err propagates and
				 * the rest of the flight is dropped instead
				 * of corrupting whatever follows the region.
				 */
				(void)atomic_cas(&g_bin_ctx.sticky_err, 0,
						 (atomic_val_t)-ENOSPC);
				LOG_ERR("bin_disk: region full at sector %u",
					BIN_DISK_OFFSET_SEC + sec);
			} else {
				int rc = disk_access_write(BIN_DISK_NAME,
					b->data,
					BIN_DISK_OFFSET_SEC + sec,
					g_bin_ctx.sectors_per_frame);

				if (rc != 0) {
					(void)atomic_cas(&g_bin_ctx.sticky_err, 0,
							 (atomic_val_t)rc);
					LOG_ERR("bin_disk: write at sector %u "
						"failed (%d)",
						BIN_DISK_OFFSET_SEC + sec, rc);
				} else {
					g_bin_ctx.cur_sector_offset =
						sec + g_bin_ctx.sectors_per_frame;
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

static void bin_frame_init(struct bin_disk_ctx *ctx, struct bin_buf *b)
{
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

static int bin_take_free(struct bin_disk_ctx *ctx, k_timeout_t to)
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

static int bin_rotate(struct bin_disk_ctx *ctx)
{
	int idx = ctx->active_idx;

	if (k_msgq_put(&bin_flush_q, &idx, K_NO_WAIT) != 0) {
		bin_bufs[idx].used = 0;
		(void)atomic_cas(&g_bin_ctx.sticky_err, 0,
				 (atomic_val_t)-EBUSY);
		return -EBUSY;
	}

	return bin_take_free(ctx,
		K_MSEC(CONFIG_DATA_LOGGER_BIN_PRODUCER_TIMEOUT_MS));
}

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

static int bin_init(struct data_logger *logger, const char *path)
{
	ARG_UNUSED(path);

	if (!atomic_cas(&g_bin_open, 0, 1)) {
		LOG_ERR("bin_disk: a binary logger is already open");
		return -EBUSY;
	}

	struct bin_disk_ctx *ctx = &g_bin_ctx;
	int idx;
	int rc;

	memset(ctx, 0, sizeof(*ctx));
	atomic_set(&ctx->sticky_err, 0);

	rc = disk_access_init(BIN_DISK_NAME);
	if (rc != 0) {
		LOG_ERR("bin_disk: disk_access_init(%s) failed (%d)",
			BIN_DISK_NAME, rc);
		atomic_set(&g_bin_open, 0);
		return rc;
	}

	uint32_t sector_size = 0;

	rc = disk_access_ioctl(BIN_DISK_NAME, DISK_IOCTL_GET_SECTOR_SIZE,
			       &sector_size);
	if (rc != 0 || sector_size == 0) {
		LOG_ERR("bin_disk: GET_SECTOR_SIZE failed (%d, sz=%u)",
			rc, sector_size);
		atomic_set(&g_bin_open, 0);
		return rc != 0 ? rc : -EIO;
	}

	if (BIN_FRAME_SIZE % sector_size != 0) {
		LOG_ERR("bin_disk: frame size %zu not a multiple of sector "
			"size %u", BIN_FRAME_SIZE, sector_size);
		atomic_set(&g_bin_open, 0);
		return -EINVAL;
	}

	ctx->sector_size       = sector_size;
	ctx->sectors_per_frame = (uint32_t)(BIN_FRAME_SIZE / sector_size);

	if (((uint32_t)BIN_DISK_SIZE_SEC % ctx->sectors_per_frame) != 0) {
		LOG_ERR("bin_disk: size-sectors %u not a multiple of frame "
			"sectors %u", (uint32_t)BIN_DISK_SIZE_SEC,
			ctx->sectors_per_frame);
		atomic_set(&g_bin_open, 0);
		return -EINVAL;
	}

	ctx->flight_id         = k_ticks_to_ns_floor64(k_uptime_ticks());
	ctx->next_seq          = 0;
	ctx->cur_sector_offset = 0;
	ctx->active_idx        = -1;

	while (k_msgq_get(&bin_flush_q, &idx, K_NO_WAIT) == 0) { }
	while (k_msgq_get(&bin_free_q,  &idx, K_NO_WAIT) == 0) { }
	for (int i = 0; i < BIN_BUF_COUNT; i++) {
		bin_bufs[i].used = 0;
		(void)k_msgq_put(&bin_free_q, &i, K_NO_WAIT);
	}

	rc = bin_take_free(ctx, K_NO_WAIT);
	if (rc != 0) {
		atomic_set(&g_bin_open, 0);
		return rc;
	}

	logger->ctx = ctx;
	return 0;
}

static int bin_write_header(struct data_logger *logger)
{
	ARG_UNUSED(logger);
	return 0;
}

static int bin_write_datapoint(struct data_logger *logger,
			       const struct datapoint *dp)
{
	struct bin_disk_ctx *ctx = logger->ctx;
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
	struct bin_disk_ctx *ctx = logger->ctx;
	int rc;

	if (bin_bufs[ctx->active_idx].used > BIN_HDR_SIZE) {
		rc = bin_rotate(ctx);
		if (rc != 0) {
			LOG_ERR("bin_disk_flush: rotate failed (%d)", rc);
			return rc;
		}
	}

	rc = bin_drain_writer();
	if (rc != 0) {
		LOG_ERR("bin_disk_flush: drain failed (%d)", rc);
		return rc;
	}

	int err = (int)atomic_get(&ctx->sticky_err);

	return err;
}

static int bin_on_event(struct data_logger *logger, enum data_logger_event ev)
{
	struct bin_disk_ctx *ctx = logger->ctx;

	if (ctx == NULL) {
		return -EINVAL;
	}

	switch (ev) {
	case DLE_BOOST:
		LOG_INF("bin_disk: BOOST at seq=%u", ctx->next_seq);
		break;
	case DLE_LANDED:
		LOG_INF("bin_disk: LANDED at seq=%u", ctx->next_seq);
		break;
	}

	return 0;
}

static int bin_close(struct data_logger *logger)
{
	struct bin_disk_ctx *ctx = logger->ctx;

	(void)bin_flush(logger);

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
/*  Converter IO                                                              */
/* -------------------------------------------------------------------------- */

static uint32_t g_io_sector_size;

int bin_io_open(void)
{
	int rc = disk_access_init(BIN_DISK_NAME);

	if (rc != 0) {
		LOG_ERR("bin_io_disk: disk_access_init(%s) failed (%d)",
			BIN_DISK_NAME, rc);
		return rc;
	}

	rc = disk_access_ioctl(BIN_DISK_NAME, DISK_IOCTL_GET_SECTOR_SIZE,
			       &g_io_sector_size);
	if (rc != 0 || g_io_sector_size == 0) {
		LOG_ERR("bin_io_disk: GET_SECTOR_SIZE failed (%d, sz=%u)",
			rc, g_io_sector_size);
		return rc != 0 ? rc : -EIO;
	}

	return 0;
}

int bin_io_close(void)
{
	g_io_sector_size = 0;
	return 0;
}

int bin_io_read(off_t off, void *buf, size_t len)
{
	if (g_io_sector_size == 0) {
		return -ENODEV;
	}
	if ((size_t)off % g_io_sector_size != 0 ||
	    len % g_io_sector_size != 0) {
		return -EINVAL;
	}

	uint32_t sector = (uint32_t)((size_t)off / g_io_sector_size) +
			  BIN_DISK_OFFSET_SEC;
	uint32_t count  = (uint32_t)(len / g_io_sector_size);

	return disk_access_read(BIN_DISK_NAME, buf, sector, count);
}

size_t bin_io_total_size(void)
{
	if (g_io_sector_size == 0) {
		return 0;
	}
	return (size_t)BIN_DISK_SIZE_SEC * g_io_sector_size;
}
