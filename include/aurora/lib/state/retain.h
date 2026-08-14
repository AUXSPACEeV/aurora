/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_STATE_RETAIN_H_
#define APP_LIB_STATE_RETAIN_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <aurora/lib/state/state.h>

/**
 * @defgroup lib_state_retain Flight state retention
 * @ingroup lib_state
 * @{
 *
 * @brief Carry the active flight state across a watchdog reset.
 *
 * A wedged flight computer that reboots into @c SM_IDLE has silently
 * disarmed itself mid-flight.
 * This module keeps the live state in RTC slow
 * memory so the machine can pick up where it left off.
 *
 * The record lives in @c .rtc_noinit, which the linker marks NOLOAD and the
 * startup code does not clear, so it survives the warm reset a watchdog
 * produces.
 *
 * Recovery is deliberately restricted to watchdog resets.
 * A reset button, a fresh power-up or a debugger attach must all land in
 * @c SM_IDLE, or the operator loses the ability to put the vehicle into a
 * known-safe state.
 */

/**
 * @brief Capacity of the opaque payload carried alongside the state.
 *
 * Sized for the attitude calibration with room to spare; RTC slow memory is
 * 8 KiB and this whole subsystem uses a fraction of it.
 */
#define SM_RETAIN_BLOB_SIZE 256

/**
 * @brief Persist @p state so a watchdog reset can resume from it.
 *
 * Called from the state core on every transition.  Writing to RTC RAM is a
 * handful of stores, so this is cheap enough for the transition path and
 * involves no flash erase.
 *
 * @param state State now active.
 */
void sm_retain_save(enum sm_state state);

/**
 * @brief Recover the flight state latched before a watchdog reset.
 *
 * Succeeds only when all of the following apply:
 *   - the reset cause latched at boot includes @c RESET_WATCHDOG,
 *   - the record carries the expected magic, version and payload size,
 *   - the CRC matches,
 *   - the record was written by the same state machine backend, and
 *   - the retained state is one a flight can actually be interrupted in
 *     (not @c SM_IDLE, which means nothing was in progress).
 *
 * @param out Receives the state to resume.  Untouched on failure.
 *
 * @retval 0 on success.
 * @retval -ENOTSUP if the reset was not caused by the watchdog.
 * @retval -ENOENT if no valid record is present.
 * @retval -EINVAL if the record is stale, corrupt or from another backend.
 */
int sm_retain_restore(enum sm_state *out);

/**
 * @brief Discard the retained record.
 *
 * Called when the machine reaches a state that must not be resumed into,
 * so a later watchdog reset starts clean.
 */
void sm_retain_invalidate(void);

/**
 * @brief Number of watchdog recoveries since the last power cycle.
 *
 * Survives in the same record.
 * A climbing count means the board is resetting repeatedly rather than
 * recovering, which is worth surfacing.
 *
 * @return Recovery count, or 0 if no valid record exists.
 */
uint16_t sm_retain_recovery_count(void);

/**
 * @brief Whether this boot resumed a flight.
 *
 * Lets the application restore anything that lives outside the state
 * machine, most importantly the attitude calibration.
 *
 * @retval true if @ref sm_retain_restore recovered a state this boot.
 */
bool sm_retain_recovered(void);

/**
 * @brief Store an opaque payload alongside the retained state.
 *
 * Used for the attitude calibration.
 * Losing it across a reset is not a cosmetic problem: calibration only runs in
 * @c SM_IDLE, so a machine that resumes into a flight state would never
 * recalibrate.
 * Orientation would integrate from a zero reference (reading horizontal, which
 * disarms on the elevation gate) and vertical acceleration would stay pinned
 * at zero, leaving boost and apogee detection blind.
 *
 * @param data Payload to copy in.
 * @param len  Payload size, at most @c SM_RETAIN_BLOB_SIZE.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p data is NULL or @p len exceeds the capacity.
 */
int sm_retain_save_blob(const void *data, size_t len);

/**
 * @brief Retrieve the payload stored by @ref sm_retain_save_blob.
 *
 * @param data Destination buffer.
 * @param len  Expected size
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p data is NULL or @p len is not the stored size.
 * @retval -ENOENT if no valid record or no payload is present.
 */
int sm_retain_load_blob(void *data, size_t len);

/** @} */

#endif /* APP_LIB_STATE_RETAIN_H_ */
