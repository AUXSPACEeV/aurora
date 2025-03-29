/**
 * @file watchdog_service.c
 * @brief Watchdog service task source file
 * @note This file contains the function implementations for servicing the
 * watchdog timer.
 *
 * Author: Maximilian Stephan @ Auxspace e.V.
 * Copyright (C) 2025 Auxspace e.V.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/watchdog.h"

#include <aurora/task/watchdog_service.h>

 /* WDT task defines */
#define WDT_RESET_TASK_PRI       (tskIDLE_PRIORITY + 1)
#define WDT_RESET_TASK_STACKSIZE (configMINIMAL_STACK_SIZE * 4)

/**
 * Watchdog reset
 */
static void x_watchdog_service_task(void* args);
static TaskHandle_t wdt_task_handle = NULL;

/*----------------------------------------------------------------------------*/

/**
 * @brief Update the watchdog timer
 *
 * @param args Unused task arguments
 * @return void
 */
static void x_watchdog_service_task(void* args)
{
    /* Service the WDT every 5 seconds */
    const TickType_t xDelay = (WDT_CNTR_MS / 2) / portTICK_PERIOD_MS;

    for (;; ) {
        watchdog_update();
        vTaskDelay(xDelay);
    }
}

/*----------------------------------------------------------------------------*/

int start_wdt_task(void)
{
    return xTaskCreate(x_watchdog_service_task, "Watchdog Service",
        WDT_RESET_TASK_STACKSIZE, NULL, WDT_RESET_TASK_PRI, &wdt_task_handle);
}

/*----------------------------------------------------------------------------*/

void init_wdt(void) {
    watchdog_enable(WDT_CNTR_MS, 1);

    if (watchdog_caused_reboot()) {
        printf("Rebooted by Watchdog!\n");
        // TODO: Determine what action we take if a watchdog caused a reboot
    }
}
