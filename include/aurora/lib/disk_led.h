/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_DISK_LED_H_
#define APP_LIB_DISK_LED_H_

/**
 * @defgroup lib_disk_led SD-card activity LED
 * @ingroup lib_notify
 * @{
 *
 * @brief SD-card activity LED, driven by the @c auxspace,disk-led chosen node.
 *
 * Separate from the PWM state-machine LED (@ref lib_notify). See the
 * "SD-card activity LED" page in the library documentation for the concept
 * and wiring.
 */

/**
 * @brief Report an SD-card access; blinks the activity LED.
 *
 * Call once per SD write. Lights the LED and re-arms a
 * @c CONFIG_AURORA_NOTIFY_DISK_LED_HOLD_MS off-timer, so frequent calls keep
 * it lit. Returns immediately; safe to call from any thread.
 */
#if defined(CONFIG_AURORA_NOTIFY_DISK_LED)
void disk_led_activity(void);
#else /* !CONFIG_AURORA_NOTIFY_DISK_LED */
static inline void disk_led_activity(void) {}
#endif /* CONFIG_AURORA_NOTIFY_DISK_LED */


/** @} */

#endif /* APP_LIB_DISK_LED_H_ */
