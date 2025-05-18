/**
 * @file none_app.c
 * @brief Aurora fake application
 * @note this app can be used for build tests, etc.
 *
 * Author: Maximilian Stephan @ Auxspace e.V.
 * Copyright (C) 2025 Auxspace e.V.
 */

#include "FreeRTOS.h"
#include "task.h"

#include <aurora/app.h>

/*----------------------------------------------------------------------------*/

__attribute__((weak)) int aurora_hwinit(void)
{
    return 0;
}

/*----------------------------------------------------------------------------*/

__attribute__((weak)) void aurora_hwdeinit(void) { return; }

/*----------------------------------------------------------------------------*/

__attribute__((weak)) void aurora_main(void)
{
    return;
}

/* EOF */