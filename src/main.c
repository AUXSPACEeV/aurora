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
#include <errno.h>
#include "pico/stdlib.h"
#include "pico/binary_info.h"
#include "hardware/watchdog.h"

/* Local includes */
#include <aurora/drivers/mmc/spi_mmc.h>
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
    int ret;

    stdio_init_all();
    init_wdt();
    spi_init(spi_default, 1000 * 1000);

    gpio_set_function(PICO_DEFAULT_SPI_RX_PIN, GPIO_FUNC_SPI);
    gpio_set_function(PICO_DEFAULT_SPI_SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(PICO_DEFAULT_SPI_TX_PIN, GPIO_FUNC_SPI);
    // Make the SPI pins available to picotool
    bi_decl(bi_3pins_with_func(PICO_DEFAULT_SPI_RX_PIN, PICO_DEFAULT_SPI_TX_PIN, PICO_DEFAULT_SPI_SCK_PIN, GPIO_FUNC_SPI));

    // Chip select is active-low, so we'll initialise it to a driven-high state
    gpio_init(PICO_DEFAULT_SPI_CSN_PIN);
    gpio_put(PICO_DEFAULT_SPI_CSN_PIN, 1);
    gpio_set_dir(PICO_DEFAULT_SPI_CSN_PIN, GPIO_OUT);
    // Make the CS pin available to picotool
    bi_decl(bi_1pin_with_name(PICO_DEFAULT_SPI_CSN_PIN, "SPI CS"));

    mmc_drv_t *mmc_drv = spi_mmc_drv_init(spi_default, PICO_DEFAULT_SPI_CSN_PIN, false);
    if (mmc_drv == NULL) {
        printf("SPI SD init failed: %d\n", -ENOMEM);
        return;
    }
    ret = mmc_drv->ops->probe(mmc_drv->dev);
    if (ret) {
        printf("SPI SD init failed: %d\n", ret);
        return;
    }

    printf("SPI initialised, let's goooooo\n");
}

/*----------------------------------------------------------------------------*/

static void prv_setup_early_tasks(void)
{
    /* Create the watchdog service task */
    int ret = start_wdt_task();
    if (ret != pdPASS)
        printf("WDT service task could not be created.\n");
}
