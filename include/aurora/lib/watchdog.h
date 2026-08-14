/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_WATCHDOG_H_
#define APP_LIB_WATCHDOG_H_

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

/** @} */

#endif /* APP_LIB_WATCHDOG_H_ */
