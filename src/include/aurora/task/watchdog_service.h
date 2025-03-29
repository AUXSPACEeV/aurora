/**
 * @file watchdog_service.h
 * @brief Watchdog service task header file
 * @note This file contains the function prototypes and definitions for
 * the watchdog timer service.
 *
 * Author: Maximilian Stephan @ Auxspace e.V.
 * Copyright (C) 2025 Auxspace e.V.
 */

#ifndef WATCHDOG_SERVICE_H
#define WATCHDOG_SERVICE_H

 /* WDT timeout in ms */
#define WDT_CNTR_MS              (10000U)

/**
 * @brief Start the watchdog service task
 *
 * @return int
 */
int start_wdt_task(void);

/**
 * @brief Initialize the watchdog timer
 *
 * @note This function enables the watchdog timer with a timeout of WDT_CNTR_MS.
 */
void init_wdt(void);

#endif /* WATCHDOG_SERVICE_H */