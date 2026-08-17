/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_WATCHDOG_H_
#define APP_LIB_WATCHDOG_H_

#include <zephyr/toolchain.h>

/**
 * @defgroup lib_watchdog Hardware watchdog supervision
 * @ingroup lib
 * @{
 *
 * @brief Reboot the board when the kernel itself stops running.
 *
 * This supervises kernel liveness, not application progress.  A dedicated
 * thread does nothing but feed a task watchdog channel on a timer; if that
 * thread stops being scheduled, the board resets.
 *
 * The deliberate limitation is that it does @b not supervise sensor data
 * flow.
 * Feeding a watchdog from the state machine's zbus loop was tried and reverted:
 * that loop waits on sensor messages, so a wedged sensor turned into a reset
 * loop, and because an I2C wedge survives a warm reset the board never
 * recovered.
 * A sensor that stops producing is a degraded flight the software should fly
 * through, not a reason to reboot.
 *
 * What this does catch is the whole class of failures where the kernel
 * stops entirely, like a clock-gated core, a double fault spinning in the
 * exception vector, none of which run any fault handler, produce any
 * console output, or reset on their own.
 *
 * @c CONFIG_TASK_WDT_HW_FALLBACK is what makes this trustworthy: the task
 * watchdog is itself driven by the kernel timer, so a stopped kernel clock
 * would stop the supervisor too.
 * The hardware watchdog behind it runs off its own clock and fires regardless.
 */

/**
 * @brief Start watchdog supervision.
 *
 * Installs the hardware watchdog behind the task watchdog and starts the
 * feeder thread.  Safe to call once, from application init.
 *
 * @retval 0 on success.
 * @retval -ENODEV if the hardware watchdog device is not ready.
 * @retval -errno as reported by the task watchdog subsystem.
 */
int aurora_watchdog_setup(void);

/**
 * @brief Liveness sources recorded in the breadcrumb.
 *
 * Purely observational.  See @ref aurora_watchdog_kick for why these must
 * never gate the feed.
 */
enum aurora_wdt_source {
	AURORA_WDT_SRC_IMU = 0, /**< IMU samples arriving at the state machine. */
	AURORA_WDT_SRC_BARO,    /**< Baro samples arriving at the state machine. */
	AURORA_WDT_SRC_STATE,   /**< State machine updates completing.          */
	AURORA_WDT_SRC_COUNT,
};

#if defined(CONFIG_AURORA_WATCHDOG_BREADCRUMB)

/**
 * @brief Record that a liveness source is still producing.
 *
 * Cheap enough for a sensor hot path: one 32-bit store of the current
 * uptime, no lock.
 *
 * This deliberately does @b not influence whether the watchdog is fed.  The
 * feeder keeps feeding no matter how long a source has been silent, because
 * an I2C wedge survives a warm reset: gating the feed on sensor flow turns a
 * degraded flight into a reset loop, which is why feeding from the state
 * machine's zbus loop was tried in July and reverted.  The kicks exist only
 * so that a reset which does happen leaves behind an answer to "what had
 * already stopped?".
 *
 * @param src Source that just made progress.
 */
void aurora_watchdog_kick(enum aurora_wdt_source src);

/**
 * @brief Log the breadcrumb left by the previous boot.
 *
 * Called automatically by @ref aurora_watchdog_setup, which snapshots the
 * record before the feeder overwrites it.  Exposed so a shell command can
 * reprint it.
 *
 * A watchdog reset runs no fault handler and produces no coredump, so
 * without this a reset carries exactly one bit of information.  The record
 * lives in RTC slow memory alongside the flight state, and carries the same
 * caveat: it survives a warm reset, not a power cycle.
 *
 * Reading the output:
 *  - a per-source silence far larger than that sensor's period says the
 *    sensor pipeline stopped first, and the reset is a consequence;
 *  - a worst feed gap that never grew beyond the nominal interval says the
 *    feeder was running normally right up to the end, so whatever stopped
 *    the kernel stopped it abruptly and completely -- a parked kernel clock
 *    or a hard lockup, not a thread gradually starving the feeder.
 */
void aurora_watchdog_report(void);

#else /* !CONFIG_AURORA_WATCHDOG_BREADCRUMB */

static inline void aurora_watchdog_kick(enum aurora_wdt_source src)
{
	ARG_UNUSED(src);
}

static inline void aurora_watchdog_report(void)
{
}

#endif /* CONFIG_AURORA_WATCHDOG_BREADCRUMB */

/** @} */

#endif /* APP_LIB_WATCHDOG_H_ */
