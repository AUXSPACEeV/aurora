/**
 * @file aurora.h
 * @brief Aurora Application Interface
 * @note This file contains the function prototypes and definitions every
 * Aurora config has to implement.
 */

#pragma once

/**
 * @brief Initialize the board specific hardware (e.g. SDCard, misc peripherals)
 *
 * @return int 0 on success, negative error code on failure
 */
int aurora_hwinit(void);

/**
 * @brief Deinitialize the board specific hardware
 */
void aurora_hwdeinit(void);

/**
 * @brief Main function of the Aurora application
 * @note This function is called by the Aurora framework to start the
 * PCB specific application.
 *
 * The task runs with FreeRTOS priority (configMAX_PRIORITIES / 2).
 * The task stack size is set to (configMINIMAL_STACK_SIZE * 0x10).
 *
 * More information about tasks can be found in main.c and the FreeRTOS
 * Documentation.
 *
 * @ref https://www.freertos.org/Documentation
 */
void aurora_main(void);

/* [] END OF FILE */