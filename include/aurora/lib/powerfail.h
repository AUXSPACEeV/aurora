/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_POWERFAIL_H_
#define APP_LIB_POWERFAIL_H_

#include <aurora/lib/state/simple.h>

/**
 * @defgroup lib_powerfail Powerfail mitigation library
 * @ingroup lib
 * @{
 *
 * @brief AURORA powerfail mitigation library for user-facing indicators.
 *
 * Provides an abstract powerfail interface to perform critical tasks shortly
 * before powerdown occurs.
 */

/** @brief Powerfail callback type, invoked from ISR context. */
typedef void (*powerfail_cb_t)(void);

/**
 * @brief Initialise the powerfail mitigation subsystem.
 *
 * @param assert_cb Optional callback invoked in ISR context after emergency
 *                  state save when power failure is detected. May be NULL.
 * @param deassert_cb Optional callback invoked in ISR context when power is
 *                    restored (pin returns to default pullup). May be NULL.
 */
void powerfail_setup(powerfail_cb_t assert_cb, powerfail_cb_t deassert_cb);

/** @} */

#endif /* APP_LIB_POWERFAIL_H_ */
