/**
 * @file display.h
 * @brief ST7789 / LVGL ground-station display.
 *
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GROUND_SUPPORT_DISPLAY_H_
#define GROUND_SUPPORT_DISPLAY_H_

/**
 * @brief Report whether the display device is ready.
 *
 * The UI is built and refreshed on the display thread (started via
 * K_THREAD_DEFINE); this only probes readiness so a missing panel is visible
 * in the log.
 *
 * @retval 0 on success.
 * @retval -ENODEV if the display device is not ready.
 */
int display_init(void);

#endif /* GROUND_SUPPORT_DISPLAY_H_ */
