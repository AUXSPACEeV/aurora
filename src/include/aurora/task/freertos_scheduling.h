/**
 * @file freertos_scheduling.h
 * @brief FreeRTOS basic scheduling tasks header file
 * @note This file contains the function prototypes and definitions for
 * the freertos scheduler.
 *
 * Author: Maximilian Stephan @ Auxspace e.V.
 * Copyright (C) 2025 Auxspace e.V.
 */

#ifndef FREERTOS_SCHEDULING_H
#define FREERTOS_SCHEDULING_H

#include "task.h"

 /**
  * @brief FreeRTOS hook function for malloc failure
  */
void vApplicationMallocFailedHook(void);

/**
 * @brief FreeRTOS hook function for idle task
 * @note This function is called on each cycle of the idle task.
 * It must not attempt to block.
 */
void vApplicationIdleHook(void);

/**
 * @brief FreeRTOS hook function for stack overflow
 */
void vApplicationStackOverflowHook(TaskHandle_t pxTask, char* pcTaskName);

/**
 * @brief FreeRTOS hook function for tick
 * @note This function is called on each tick interrupt.
 */
void vApplicationTickHook(void);

#endif /* FREERTOS_SCHEDULING_H */
