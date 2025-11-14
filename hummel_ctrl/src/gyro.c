/*
 * Copyright (c) 2025, Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

#include <lib/gyro.h>

int gyro_init(const struct device *dev) {
  if (!device_is_ready(dev)) {
    printk("%s: device not ready.\n", dev->name);
    return -ENODEV;
  }

  return 0;
}

