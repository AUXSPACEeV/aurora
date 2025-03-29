/**
 * AURORA - AUxspace ROcket opeRAting System
 * Copyright (C) 2025 2025 Auxspace e.V.
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
#include "pico/stdlib.h"
#include "hardware/watchdog.h"

/* Local includes */
#include <aurora/task/freertos_scheduling.h>
#include <aurora/task/watchdog_service.h>

/**
 * Configure the hardware as necessary
 */
static void prv_setup_hardware(void);

/**
 * Setup required early tasks
 */
static void prv_setup_early_tasks(void);

/*----------------------------------------------------------------------------*/

int main(void)
{
    /* Configure the hardware ready to run the demo. */
    prv_setup_hardware();
    prv_setup_early_tasks();

    vTaskStartScheduler();

    return 0;
}

/*----------------------------------------------------------------------------*/

static void prv_setup_hardware(void)
{
    stdio_init_all();
    init_wdt();
}

/*----------------------------------------------------------------------------*/

static void prv_setup_early_tasks(void)
{
    /* Create the watchdog service task */
    int ret = start_wdt_task();
    if (ret != pdPASS)
        printf("WDT service task could not be created.\n");
}
