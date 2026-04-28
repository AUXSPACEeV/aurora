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

#endif /* AURORA_LIB_DATA_BIN_IO_H_ */
