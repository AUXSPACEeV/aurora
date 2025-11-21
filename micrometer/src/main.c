/*
 * Copyright (c) 2025 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/devicetree.h>
#include <zephyr/device.h>

#include <app_version.h>

#if defined(CONFIG_STORAGE)
#include <ff.h>
#include <zephyr/fs/fs.h>
#include <lib/storage.h>
#endif /* CONFIG_STORAGE */

#if defined(CONFIG_USB_SERIAL)
#include <lib/usb_serial.h>
#endif /* CONFIG_USB_SERIAL */

#if defined(CONFIG_IMU)
#include <lib/imu.h>
#endif /* CONFIG_IMU */

#if defined(CONFIG_BARO)
#include <lib/baro.h>
#endif /* CONFIG_BARO */

LOG_MODULE_REGISTER(main, CONFIG_MICROMETER_LOG_LEVEL);

/* ============================================================
 *                     IMU TASK
 * ============================================================ */
#if defined(CONFIG_IMU)

void imu_task(void *, void *, void *)
{
    const struct device *mpu6050 = DEVICE_DT_GET_ONE(invensense_mpu6050);

    imu_init(mpu6050);

    while (1) {
        int rc = imu_poll(mpu6050);
        if (rc != 0) {
            LOG_ERR("IMU polling failed (%d)", rc);
            break;
        }

        k_sleep(K_SECONDS(2));
    }

    LOG_INF("IMU task stopped.");
}

/* Create the IMU task (inactive unless CONFIG_IMU=y) */
K_THREAD_DEFINE(imu_task_id, 2048, imu_task, NULL, NULL, NULL,
                5, 0, 0);

#endif /* CONFIG_IMU */


/* ============================================================
 *                     BARO TASK
 * ============================================================ */
#if defined(CONFIG_BARO)

void baro_task(void *, void *, void *)
{
    const struct device *bmp0 = DEVICE_DT_GET(DT_NODELABEL(bmp180_0));
    const struct device *bmp1 = DEVICE_DT_GET(DT_NODELABEL(bmp180_1));

    if (!device_is_ready(bmp0) || !device_is_ready(bmp1)) {
        LOG_ERR("One of BMP180 sensors not ready!");
        return;
    }

    struct sensor_value temp, press;

    while (1) {

        if (baro_measure(bmp0, &temp, &press)) {
            LOG_ERR("Failed to measure BMP0");
            continue;
        }

        LOG_INF("[BMP0] Temp: %.1f C | Press: %.1f kPa",
                sensor_value_to_double(&temp),
                sensor_value_to_double(&press) / 1000.0);

        if (baro_measure(bmp1, &temp, &press)) {
            LOG_ERR("Failed to measure BMP1");
            continue;
        }

        LOG_INF("[BMP1] Temp: %.1f C | Press: %.1f kPa",
                sensor_value_to_double(&temp),
                sensor_value_to_double(&press) / 1000.0);

        k_sleep(K_SECONDS(1));
    }
}

/* Create the BARO task */
K_THREAD_DEFINE(baro_task_id, 2048, baro_task, NULL, NULL, NULL,
                5, 0, 0);

#endif /* CONFIG_BARO */


/* ============================================================
 *                     MAIN INITIALIZATION
 * ============================================================ */
int main(void)
{
    int ret;

#if defined(CONFIG_USB_SERIAL)
    ret = init_usb_serial();
    if (ret) {
        LOG_ERR("Could not initialize USB Serial (%d)", ret);
        return 1;
    }
#endif

    LOG_INF("Auxspace Micrometer %s", APP_VERSION_STRING);

#if defined(CONFIG_STORAGE)
    ret = storage_init();
    if (ret) {
        LOG_ERR("Could not initialize storage (%d)", ret);
        return 1;
    }

    /* init storage and create directories/files ... */
    /* (your existing code unchanged) */
#endif

    LOG_INF("Initialization complete. Starting sensor tasks...");

    /* Threads start automatically via K_THREAD_DEFINE */

    return 0;
}
