/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_WATCHDOG_H_
#define APP_LIB_WATCHDOG_H_

#include <stdint.h>

/**
 * @defgroup lib_watchdog Watchdog supervision library
 * @ingroup lib
 * @{
 *
 * @brief AURORA hardware-watchdog supervision for critical threads.
 *
 * Wraps the SoC hardware watchdog (e.g. the ESP32-S3 TIMG0 MWDT exposed as the
 * @c auxspace,wdt chosen node) behind a software supervisor. Critical threads
 * register once and "check in" (feed) on every loop iteration. A dedicated
 * monitor thread refreshes the hardware watchdog only while every registered
 * task has checked in within its individual deadline; as soon as one task
 * stalls the monitor withholds the feed and the hardware watchdog resets the
 * SoC.
 *
 * This turns the single-channel hardware watchdog into per-thread supervision
 * without dynamic allocation.
 */

/** @brief Handle returned by aurora_wdt_register(); < 0 on failure. */
typedef int aurora_wdt_task_t;

/** @brief Sentinel for an invalid/unregistered task handle. */
#define AURORA_WDT_TASK_INVALID (-1)

/**
 * @brief Arm the hardware watchdog and start the supervisor thread.
 *
 * Installs a reset-on-expiry timeout on the @c auxspace,wdt device and spawns
 * the monitor thread. Until the first task registers the monitor keeps feeding
 * the hardware watchdog, so the (potentially slow) boot/init window is not
 * supervised.
 *
 * @retval 0 on success.
 * @retval -ENODEV if the watchdog device is not ready.
 * @retval <0 propagated from wdt_install_timeout()/wdt_setup().
 */
int aurora_wdt_init(void);

/**
 * @brief Register a critical thread for watchdog supervision.
 *
 * Call once, typically right before the thread enters its main loop (after any
 * blocking initialisation). @p name must have static lifetime; it is stored by
 * pointer and used in log/reset diagnostics.
 *
 * @param name        Human-readable task name (static storage).
 * @param deadline_ms Maximum time allowed between check-ins before the task is
 *                    considered stalled.
 * @retval >=0 task handle to pass to aurora_wdt_feed().
 * @retval AURORA_WDT_TASK_INVALID if no free slot is available.
 */
aurora_wdt_task_t aurora_wdt_register(const char *name, uint32_t deadline_ms);

/**
 * @brief Check in a registered task ("kick" the software watchdog).
 *
 * Records the current uptime as the task's last healthy point. Should be called
 * at least once per @c deadline_ms from within the supervised thread.
 *
 * @param task Handle from aurora_wdt_register().
 * @retval 0 on success.
 * @retval -EINVAL if @p task is invalid or not registered.
 */
int aurora_wdt_feed(aurora_wdt_task_t task);

/**
 * @brief Stop supervising a task and free its slot.
 *
 * @param task Handle from aurora_wdt_register().
 * @retval 0 on success.
 * @retval -EINVAL if @p task is invalid.
 */
int aurora_wdt_unregister(aurora_wdt_task_t task);

/**
 * @brief Hook invoked once when the supervisor decides to reset the SoC.
 *
 * Called from the monitor thread the moment it starts withholding the feed
 * because a task missed its deadline i.e. one timeout period before the
 * hardware watchdog actually resets. The default implementation is a no-op;
 * override it (e.g. wdt_recovery library does) to persist state before reset.
 * Runs in the monitor thread context, so it may take locks but must be brief.
 */
void aurora_wdt_reset_imminent(void);

/** @} */

#endif /* APP_LIB_WATCHDOG_H_ */
