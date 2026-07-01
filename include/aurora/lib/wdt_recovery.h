/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_WDT_RECOVERY_H_
#define APP_LIB_WDT_RECOVERY_H_

#include <stdbool.h>
#include <aurora/lib/state/simple.h>

/**
 * @defgroup lib_wdt_recovery Watchdog recovery library
 * @ingroup lib
 * @{
 *
 * @brief Persist just enough flight state to resume after a watchdog reset.
 *
 * "Recovery" here is watchdog recovery of the firmware's flight state — not the
 * vehicle's physical recovery (parachutes / pyros). The subsystem keeps a tiny
 * record (the last flight state plus a software-declared "watchdog imminent"
 * marker) in a retained-memory area that survives a warm reset but is cleared
 * on real power loss. On boot it combines that record with the hardware reset
 * cause (@c hwinfo) to decide whether the board was rebooted by the watchdog
 * mid-flight, and if so hands the last state back so the application can resume
 * instead of starting cold in @c SM_IDLE.
 *
 * Because a full power cycle clears the retained record, recovery only ever
 * triggers after a warm reset — the board never resurrects a stale flight state
 * after being powered off and on again on the ground.
 *
 * The record is validated by a magic prefix and checksum (see the
 * @c zephyr,retention devicetree node), so a cold boot, a corrupted region, or
 * a board without a recovery backend all degrade safely to "no recovery".
 */

/** @brief Reason the board last rebooted, resolved at aurora_wdt_recovery_init(). */
enum aurora_reboot_reason {
	AURORA_REBOOT_UNKNOWN = 0, /**< Cause could not be determined. */
	AURORA_REBOOT_POWER_ON,    /**< Power-on / brown-out reset (cold boot). */
	AURORA_REBOOT_PIN,         /**< External reset pin. */
	AURORA_REBOOT_SOFTWARE,    /**< Software-requested reset. */
	AURORA_REBOOT_WATCHDOG,    /**< Watchdog reset. */
	AURORA_REBOOT_DEBUG,       /**< Debugger-induced reset. */
};

/**
 * @brief Resolve the reboot reason and decide whether recovery is pending.
 *
 * Reads the hardware reset cause and the retained record, resolves the reboot
 * reason, and latches a pending recovery when the reset was a watchdog reset
 * and the saved state is worth resuming (not @c SM_IDLE or @c SM_ERROR). Call
 * once early in @c main().
 *
 * @retval 0 always (a missing/invalid backend simply means "no recovery").
 */
int aurora_wdt_recovery_init(void);

/**
 * @brief Reboot reason resolved by the last aurora_wdt_recovery_init().
 *
 * @return The resolved @ref aurora_reboot_reason.
 */
enum aurora_reboot_reason aurora_wdt_recovery_reason(void);

/**
 * @brief Query whether a watchdog recovery is pending.
 *
 * @param[out] state_out If non-NULL and recovery is pending, receives the
 *                       flight state to restore.
 * @retval true  A watchdog reset was detected with a resumable saved state.
 * @retval false No recovery pending; boot normally from @c SM_IDLE.
 */
bool aurora_wdt_recovery_pending(enum sm_state *state_out);

/**
 * @brief Persist the current flight state to the retained record.
 *
 * Cheap (a RAM write); call on every state transition so the record always
 * reflects the latest flight state.
 *
 * @param state Current flight state to persist.
 */
void aurora_wdt_recovery_save_state(enum sm_state state);

/** @} */

#endif /* APP_LIB_WDT_RECOVERY_H_ */
