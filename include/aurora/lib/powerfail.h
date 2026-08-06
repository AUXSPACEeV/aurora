/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_POWERFAIL_H_
#define APP_LIB_POWERFAIL_H_

#include <aurora/lib/state/state.h>

/**
 * @defgroup lib_powerfail Powerfail mitigation library
 * @ingroup lib
 * @{
 *
 * @brief AURORA powerfail mitigation library.
 *
 * Detects that the board is about to lose power and performs critical tasks,
 * such as stopping the data loggers so the storage card is not left mid-write
 * and raising a notification shortly before the power collapses.
 *
 * Detection comes from a dedicated power-fail monitor line behind the
 * @c auxspace,pfm chosen node, a supply supervisor's power-fail output, or a
 * comparator on the bulk capacitor, which is asserted while power is failing.
 * The board has to provide that line: an ADC cannot raise an interrupt when a
 * measurement crosses a threshold, so a battery-sense divider alone gives no
 * warning early enough to act on.
 */

/** @brief Powerfail callback type, invoked from ISR context. */
typedef void (*powerfail_cb_t)(void);

/**
 * @brief Initialise the powerfail mitigation subsystem.
 *
 * Samples the monitor line once before returning, so a board that boots with
 * power already failing does not wait for an edge that has been and gone.
 *
 * @param assert_cb Optional callback invoked in ISR context after emergency
 *                  state save when power failure is detected. May be NULL.
 * @param deassert_cb Optional callback invoked in ISR context when power is
 *                    restored (pin returns to default pullup). May be NULL.
 *
 * @retval 0 on success.
 * @retval -ENODEV if the GPIO controller is not ready.
 * @retval -errno as reported by the GPIO driver.
 */
int powerfail_setup(powerfail_cb_t assert_cb, powerfail_cb_t deassert_cb);

/** @} */

#endif /* APP_LIB_POWERFAIL_H_ */
