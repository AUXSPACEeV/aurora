/**
 * AURORA - AUxspace ROcket opeRAting System
 * Copyright (C) 2025 Auxspace e.V.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Maintainer: Maximilian Stephan @ Auxspace e.V.
 */

 /* FreeRTOS include files. */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

/* Library includes. */
#include <stdio.h>
#include <errno.h>
#include "pico/stdlib.h"
#include "pico/binary_info.h"
#include "hardware/watchdog.h"

/* Local includes */
#include <aurora/app.h>
#include <aurora/log.h>
#include <aurora/macros.h>
#include <aurora/task/freertos_scheduling.h>
#include <aurora/task/watchdog_service.h>

/* main task defines */
#define MAIN_TASK_PRI       (configMAX_PRIORITIES / 2)
#define MAIN_TASK_STACKSIZE (configMINIMAL_STACK_SIZE * 0x10)

/**
 * @brief Early setup of system critical tasks
 */
static void prv_setup_early_tasks(void);

/*----------------------------------------------------------------------------*/

/**
 * @brief Main Task
 *
 * @param args Unused task arguments
 */
static void x_main_task(void* args);
static TaskHandle_t main_task_handle = NULL;

/*----------------------------------------------------------------------------*/

/**
 * @brief Aurora main entrypoint
 * 
 * @return Aurora return / error code
 */
int main(void)
{
    int ret;

    volatile int catchme = 0;
    for (;;)
        if (catchme)
            break;

    /* Configure the hardware ready to run the demo. */
    stdio_init_all();
    init_wdt();

    prv_setup_early_tasks();

    ret = xTaskCreate(x_main_task, "Aurora Main Task", MAIN_TASK_STACKSIZE,
                      NULL, MAIN_TASK_PRI, &main_task_handle);
    if (unlikely(ret != pdPASS)) {
        log_error("Main task could not be created.\n");
        return ret;
    }

    vTaskStartScheduler();

    log_warning("Aurora finished SMP processes. Exiting...\n");

    return 0;
}

/*----------------------------------------------------------------------------*/

static void prv_setup_early_tasks(void)
{
    /* Create the watchdog service task */
    int ret = start_wdt_task();
    if (unlikely(ret != pdPASS))
        log_error("WDT service task could not be created.\n");
}

/*----------------------------------------------------------------------------*/

static void x_main_task(void __attribute__((unused)) *args)
{
    /* Wait 5 seconds */
    int ret;
    const TickType_t xDelay = (5000 / 2) / portTICK_PERIOD_MS;

    vTaskDelay(xDelay);

    printf("\r\n\r\n");
    log_info("Welcome to AURORA!");
    log_info("==================");

    ret = aurora_hwinit();
    if (ret != 0) {
        log_error("App specific hardware init failed: %d", ret);
        return;
    }

    aurora_main();

    /* Finally, after main ran through */

    aurora_hwdeinit();
}
