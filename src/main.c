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

/* WDT task defines */
#define WDT_RESET_TASK_PRI       (tskIDLE_PRIORITY + 1)
#define WDT_RESET_TASK_STACKSIZE (configMINIMAL_STACK_SIZE * 4)
#define WDT_CNTR_MS              (10000U)

/**
 * Configure the hardware as necessary
 */
static void prvSetupHardware(void);

/**
 * Prototypes for the standard FreeRTOS callback/hook functions implemented
 * within this file.
 */
void vApplicationMallocFailedHook(void);
void vApplicationIdleHook(void);
void vApplicationStackOverflowHook(TaskHandle_t pxTask, char* pcTaskName);
void vApplicationTickHook(void);

/**
 * Watchdog reset
 */
static void xWatchdogServiceTask(void* args);
static TaskHandle_t wdtTaskHandle = NULL; // globally accessible

/*-----------------------------------------------------------*/

int main(void)
{
    int ret;

    /* Configure the hardware ready to run the demo. */
    prvSetupHardware();

    if (watchdog_caused_reboot()) {
        printf("Rebooted by Watchdog!\n");
        // TODO: Determine what action we take if a watchdog caused a reboot
    }

    watchdog_enable(WDT_CNTR_MS, 1);

    ret = xTaskCreate(xWatchdogServiceTask, "Watchdog Service", WDT_RESET_TASK_STACKSIZE, NULL, WDT_RESET_TASK_PRI, &wdtTaskHandle);
    if (ret != pdPASS)
        printf("WDT service task could not be created.\n");

    vTaskStartScheduler();

    return 0;
}

/*-----------------------------------------------------------*/

static void prvSetupHardware(void)
{
    stdio_init_all();
}

/*-----------------------------------------------------------*/

void vApplicationMallocFailedHook(void)
{
    /* Called if a call to pvPortMalloc() fails because there is insufficient
    free memory available in the FreeRTOS heap.  pvPortMalloc() is called
    internally by FreeRTOS API functions that create tasks, queues, software
    timers, and semaphores.  The size of the FreeRTOS heap is set by the
    configTOTAL_HEAP_SIZE configuration constant in FreeRTOSConfig.h. */

    /* Force an assert. */
    configASSERT((volatile void*)NULL);
}

/*-----------------------------------------------------------*/

void vApplicationStackOverflowHook(TaskHandle_t pxTask, char* pcTaskName)
{
    (void)pcTaskName;
    (void)pxTask;

    /* Run time stack overflow checking is performed if
    configCHECK_FOR_STACK_OVERFLOW is defined to 1 or 2.  This hook
    function is called if a stack overflow is detected. */

    /* Force an assert. */
    configASSERT((volatile void*)NULL);
}

/*-----------------------------------------------------------*/

void vApplicationIdleHook(void)
{
    volatile size_t xFreeHeapSpace;

    /* This is just a trivial example of an idle hook.  It is called on each
    cycle of the idle task.  It must *NOT* attempt to block.  In this case the
    idle task just queries the amount of FreeRTOS heap that remains.  See the
    memory management section on the http://www.FreeRTOS.org web site for memory
    management options.  If there is a lot of heap memory free then the
    configTOTAL_HEAP_SIZE value in FreeRTOSConfig.h can be reduced to free up
    RAM. */
    xFreeHeapSpace = xPortGetFreeHeapSize();

    /* Remove compiler warning about xFreeHeapSpace being set but never used. */
    (void)xFreeHeapSpace;
}

/*-----------------------------------------------------------*/

void vApplicationTickHook(void)
{
    /* Do nothing ATM */
}

/*-----------------------------------------------------------*/

static void xWatchdogServiceTask(void* args) {
    /* Service the WDT every 5 seconds */
    const TickType_t xDelay = (WDT_CNTR_MS / 2) / portTICK_PERIOD_MS;
    for (;; ) {
        watchdog_update();
        vTaskDelay(xDelay);
    }
}
