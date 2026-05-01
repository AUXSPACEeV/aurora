/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Backend-private IO surface used by convert.c.  Exactly one of
 * fmt_bin.c (flash backend) or fmt_bin_disk.c (disk backend) is built
 * into a given image; that file provides these symbols.
 *
 * The converter never assumes a specific underlying medium; it only
 * needs sized random reads and a known total region size.
 */

#ifndef AURORA_LIB_DATA_BIN_IO_H_
#define AURORA_LIB_DATA_BIN_IO_H_

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/**
 * Open the underlying medium for converter reads.  Idempotent with
 * respect to the writer; conversion is only invoked after the live
 * logger has been closed, so there is no concurrent access.
 */
int bin_io_open(void);

/** Release any resources acquired by bin_io_open(). */
int bin_io_close(void);

/**
 * Read @p len bytes starting at byte offset @p off into @p buf.  Both
 * @p off and @p len are guaranteed to be multiples of the frame size,
 * so disk-style backends can translate cleanly to sector-aligned reads.
 */
int bin_io_read(off_t off, void *buf, size_t len);

/** Total size of the flight-log region in bytes. */
size_t bin_io_total_size(void);

/**
 * Optional fast path for locating the start of the captured window.
 *
 * Backends whose on-storage layout makes the window start trivially
 * known (e.g. the disk backend writes purely linearly from offset 0,
 * so slot 0 is always the start of the most recent flight) implement
 * this and return 0 with the outputs filled in.  Backends without a
 * cheap shortcut (e.g. the circular flash backend) return -ENOTSUP
 * and the converter falls back to scanning every slot.
 *
 * Returning -ENOENT means the backend looked but found no valid
 * frame and the converter should treat the log as empty.  Other
 * negative values are propagated as read errors.
 */
int bin_io_window_start_hint(off_t *out_offset,
			     uint32_t *out_seq,
			     uint64_t *out_flight_id);

#endif /* AURORA_LIB_DATA_BIN_IO_H_ */
