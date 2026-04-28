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
 * Buffering: a single contiguous power-of-two ring of BIN_FRAME_SIZE
 * slots sits in RAM.  The producer fills the slot at @c head; on
 * rotate it does an atomic increment of @c head and signals the
 * writer.  The writer pulls all contiguous committed frames between
 * @c tail and @c head (capped by physical-ring wrap and a max-batch
 * knob) and hands them to one @c disk_access_write call.  This both
 * drops the per-frame producer mutex and lets the SD card's FTL see
 * large multi-block transfers, which it handles dramatically better
 * under garbage-collection stalls than a stream of single-frame
 * writes.  The on-storage frame layout (header + records) is
 * identical to the flash backend, so the converter walks both with
 * the same algorithm.
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
#include <zephyr/sys/util.h>

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
/*  Frame & ring geometry                                                     */
/* -------------------------------------------------------------------------- */

#define BIN_REC_SIZE         ((size_t)sizeof(struct aurora_bin_record))
#define BIN_HDR_SIZE         ((size_t)sizeof(struct aurora_bin_frame_header))
#define BIN_FRAME_SIZE       ((size_t)CONFIG_DATA_LOGGER_BIN_FRAME_SIZE)
#define BIN_BUF_ALIGN        CONFIG_DATA_LOGGER_BIN_BUF_ALIGN
#define BIN_RING_FRAMES      ((uint32_t)CONFIG_DATA_LOGGER_BIN_RING_FRAMES)
#define BIN_RING_MASK        (BIN_RING_FRAMES - 1U)
#define BIN_RING_BYTES       (BIN_RING_FRAMES * BIN_FRAME_SIZE)
#define BIN_MAX_BATCH_FRAMES ((uint32_t)CONFIG_DATA_LOGGER_BIN_MAX_BATCH_FRAMES)

BUILD_ASSERT(BIN_FRAME_SIZE >= BIN_HDR_SIZE + BIN_REC_SIZE,
	     "frame must hold at least one header + one record");
BUILD_ASSERT((BIN_FRAME_SIZE - BIN_HDR_SIZE) % BIN_REC_SIZE == 0,
	     "frame payload should be a whole number of records");
BUILD_ASSERT(BIN_RING_FRAMES >= 2U,
	     "ring needs at least 2 frame slots");
BUILD_ASSERT((BIN_RING_FRAMES & BIN_RING_MASK) == 0U,
	     "BIN_RING_FRAMES must be a power of two");
BUILD_ASSERT(BIN_MAX_BATCH_FRAMES >= 1U,
	     "BIN_MAX_BATCH_FRAMES must be at least 1");

static uint8_t bin_ring[BIN_RING_BYTES] __aligned(BIN_BUF_ALIGN);

static inline uint8_t *frame_ptr(uint32_t idx)
{
	return &bin_ring[(idx & BIN_RING_MASK) * BIN_FRAME_SIZE];
}

struct bin_disk_ctx {
	uint64_t flight_id;
	uint32_t next_seq;          /* producer side */
	uint32_t cur_sector_offset; /* writer side: sectors past disk offset */
	uint32_t sector_size;       /* queried at init from DISK_IOCTL */
	uint32_t sectors_per_frame; /* BIN_FRAME_SIZE / sector_size */
	size_t   prod_used;         /* bytes filled in the slot at head */
	atomic_t head;              /* next frame to commit (producer-owned slot) */
	atomic_t tail;              /* next frame to write to disk */
	atomic_t sticky_err;
};

static struct bin_disk_ctx g_bin_ctx;
static atomic_t g_bin_open = ATOMIC_INIT(0);

/* Writer wakes when the producer commits a frame or flush is requested. */
K_SEM_DEFINE(bin_data_sem,  0, K_SEM_MAX_LIMIT);
/* Producer wakes when the writer drains one or more committed frames. */
K_SEM_DEFINE(bin_space_sem, 0, K_SEM_MAX_LIMIT);
/* Drain handshake for bin_flush(). */
K_SEM_DEFINE(bin_drain_sem, 0, 1);
static atomic_t bin_drain_req = ATOMIC_INIT(0);

/* -------------------------------------------------------------------------- */
/*  Writer thread                                                             */
/* -------------------------------------------------------------------------- */

static void bin_writer_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	for (;;) {
		(void)k_sem_take(&bin_data_sem, K_FOREVER);

		if (!atomic_get(&g_bin_open)) {
			continue;
		}

		uint32_t head  = (uint32_t)atomic_get(&g_bin_ctx.head);
		uint32_t tail  = (uint32_t)atomic_get(&g_bin_ctx.tail);
		uint32_t avail = head - tail;

		if (avail > 0U) {
			uint32_t to_wrap = BIN_RING_FRAMES - (tail & BIN_RING_MASK);
			uint32_t batch   = MIN(avail, to_wrap);

			batch = MIN(batch, BIN_MAX_BATCH_FRAMES);

			uint32_t sec      = g_bin_ctx.cur_sector_offset;
			uint32_t n_sec    = batch * g_bin_ctx.sectors_per_frame;

			if (sec + n_sec > (uint32_t)BIN_DISK_SIZE_SEC) {
				/* Linear region exhausted. Drop the batch so
				 * the producer doesn't hang; sticky_err will
				 * propagate and the rest of the flight is
				 * silently discarded.
				 */
				(void)atomic_cas(&g_bin_ctx.sticky_err, 0,
						 (atomic_val_t)-ENOSPC);
				LOG_ERR("bin_disk: region full at sector %u",
					BIN_DISK_OFFSET_SEC + sec);
			} else {
				int rc = disk_access_write(BIN_DISK_NAME,
					frame_ptr(tail),
					BIN_DISK_OFFSET_SEC + sec,
					n_sec);

				if (rc != 0) {
					(void)atomic_cas(&g_bin_ctx.sticky_err, 0,
							 (atomic_val_t)rc);
					LOG_ERR("bin_disk: write at sector %u "
						"(n=%u) failed (%d)",
						BIN_DISK_OFFSET_SEC + sec,
						n_sec, rc);
				} else {
					g_bin_ctx.cur_sector_offset = sec + n_sec;
				}
			}

			(void)atomic_add(&g_bin_ctx.tail, (atomic_val_t)batch);

			for (uint32_t i = 0; i < batch; i++) {
				k_sem_give(&bin_space_sem);
			}

			/* If more frames are queued (or remained after wrap),
			 * keep the writer hot.
			 */
			head = (uint32_t)atomic_get(&g_bin_ctx.head);
			tail = (uint32_t)atomic_get(&g_bin_ctx.tail);
			if (head != tail) {
				k_sem_give(&bin_data_sem);
			}
		}

		if (atomic_get(&bin_drain_req)) {
			head = (uint32_t)atomic_get(&g_bin_ctx.head);
			tail = (uint32_t)atomic_get(&g_bin_ctx.tail);
			if (head == tail) {
				atomic_set(&bin_drain_req, 0);
				k_sem_give(&bin_drain_sem);
			}
		}
	}
}

K_THREAD_DEFINE(bin_writer_th, CONFIG_DATA_LOGGER_BIN_WRITER_STACK_SIZE,
		bin_writer_fn, NULL, NULL, NULL,
		CONFIG_DATA_LOGGER_BIN_WRITER_PRIO, 0, 0);

/* -------------------------------------------------------------------------- */
/*  Producer-side helpers                                                     */
/* -------------------------------------------------------------------------- */

static void bin_frame_init(struct bin_disk_ctx *ctx, uint8_t *frame)
{
	memset(frame, 0xFF, BIN_FRAME_SIZE);

	struct aurora_bin_frame_header *h =
		(struct aurora_bin_frame_header *)frame;

	memcpy(h->magic, AURORA_BIN_FRAME_MAGIC, sizeof(h->magic));
	h->version    = AURORA_BIN_VERSION;
	h->reserved0  = 0;
	h->reserved1  = 0;
	h->seq        = ctx->next_seq++;
	h->flight_id  = ctx->flight_id;
	h->base_ts_ns = k_ticks_to_ns_floor64(k_uptime_ticks());

	ctx->prod_used = BIN_HDR_SIZE;
}

static int bin_wait_space(struct bin_disk_ctx *ctx, k_timeout_t to)
{
	for (;;) {
		uint32_t head = (uint32_t)atomic_get(&ctx->head);
		uint32_t tail = (uint32_t)atomic_get(&ctx->tail);

		/* Capacity is BIN_RING_FRAMES - 1 committed frames; the head
		 * slot is always reserved for the producer to fill.
		 */
		if ((head - tail) < (BIN_RING_FRAMES - 1U)) {
			return 0;
		}

		int rc = k_sem_take(&bin_space_sem, to);

		if (rc != 0) {
			return rc;
		}
	}
}

static int bin_rotate(struct bin_disk_ctx *ctx)
{
	/* Commit the current head frame. */
	(void)atomic_inc(&ctx->head);
	k_sem_give(&bin_data_sem);

	/* Wait for room to reserve the next head slot. */
	int rc = bin_wait_space(ctx,
		K_MSEC(CONFIG_DATA_LOGGER_BIN_PRODUCER_TIMEOUT_MS));

	if (rc != 0) {
		(void)atomic_cas(&ctx->sticky_err, 0,
				 (atomic_val_t)-EBUSY);
		ctx->prod_used = 0;
		return -EBUSY;
	}

	uint32_t head = (uint32_t)atomic_get(&ctx->head);

	bin_frame_init(ctx, frame_ptr(head));
	return 0;
}

static int bin_drain_writer(void)
{
	k_sem_reset(&bin_drain_sem);
	atomic_set(&bin_drain_req, 1);
	k_sem_give(&bin_data_sem);

	if (k_sem_take(&bin_drain_sem,
		       K_MSEC(CONFIG_DATA_LOGGER_BIN_FLUSH_TIMEOUT_MS)) != 0) {
		atomic_set(&bin_drain_req, 0);
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
	int rc;

	memset(ctx, 0, sizeof(*ctx));
	atomic_set(&ctx->sticky_err, 0);
	atomic_set(&ctx->head, 0);
	atomic_set(&ctx->tail, 0);

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

	k_sem_reset(&bin_data_sem);
	k_sem_reset(&bin_space_sem);
	k_sem_reset(&bin_drain_sem);
	atomic_set(&bin_drain_req, 0);

	bin_frame_init(ctx, frame_ptr(0));

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

	if (ctx->prod_used + BIN_REC_SIZE > BIN_FRAME_SIZE) {
		int rc = bin_rotate(ctx);

		if (rc != 0) {
			return rc;
		}
	}

	uint32_t head    = (uint32_t)atomic_get(&ctx->head);
	uint8_t *frame   = frame_ptr(head);
	struct aurora_bin_frame_header *h =
		(struct aurora_bin_frame_header *)frame;
	struct aurora_bin_record *rec =
		(struct aurora_bin_record *)(frame + ctx->prod_used);

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

static int bin_flush(struct data_logger *logger)
{
	struct bin_disk_ctx *ctx = logger->ctx;
	int rc;

	if (ctx->prod_used > BIN_HDR_SIZE) {
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
	(void)bin_flush(logger);

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
