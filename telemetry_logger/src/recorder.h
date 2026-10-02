/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TELEMETRY_LOGGER_RECORDER_H_
#define TELEMETRY_LOGGER_RECORDER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Counters for the SD-card recorder. */
struct recorder_stats {
	uint32_t recorded;   /**< Frames written to the open recording. */
	uint32_t dropped;    /**< Frames that found the queue full. */
	uint32_t unrecorded; /**< Frames that arrived with no recording open. */
	uint32_t write_errs; /**< Write or sync failures (each closes the file). */
	uint32_t sessions;   /**< Recordings opened since boot. */
	bool open;           /**< A recording is open right now. */
	uint32_t index;      /**< Index of the open (or last) recording. */
	uint64_t bytes;      /**< Bytes written to the open recording's .BIN. */
};

/**
 * @brief Start the recorder.
 *
 * Never fails on a missing card: the recorder thread keeps retrying in
 * the background, and frames that arrive meanwhile are counted as
 * unrecorded.
 *
 * @retval 0 always.
 */
int recorder_init(void);

/**
 * @brief Hand one validated frame to the recorder.
 *
 * Never blocks; safe from the link parser thread.
 *
 * @param frame Frame bytes, starting at magic0.
 * @param len   Frame length in bytes.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p frame is NULL, or @p len is zero or too long.
 * @retval -ENOBUFS if the queue is full; the frame is counted as dropped.
 */
int recorder_submit(const uint8_t *frame, size_t len);

/** @brief Snapshot the recorder counters. */
void recorder_get_stats(struct recorder_stats *out);

#endif /* TELEMETRY_LOGGER_RECORDER_H_ */
