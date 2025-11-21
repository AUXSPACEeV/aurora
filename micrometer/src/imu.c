/*
 * Copyright (c) 2025, Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <stdio.h>
#include <zephyr/drivers/sensor.h>

#include <lib/imu.h>

static const char *now_str(void)
{
	static char buf[16]; /* ...HH:MM:SS.MMM */
	uint32_t now = k_uptime_get_32();
	unsigned int ms = now % MSEC_PER_SEC;
	unsigned int s;
	unsigned int min;
	unsigned int h;

	now /= MSEC_PER_SEC;
	s = now % 60U;
	now /= 60U;
	min = now % 60U;
	now /= 60U;
	h = now;

	snprintf(buf, sizeof(buf), "%u:%02u:%02u.%03u",
		 h, min, s, ms);
	return buf;
}

int imu_poll(const struct device *dev)
{
	struct sensor_value temperature;
	struct sensor_value accel[3];
	struct sensor_value gyro[3];
	int rc = sensor_sample_fetch(dev);

	if (rc == 0) {
		rc = sensor_channel_get(dev, SENSOR_CHAN_ACCEL_XYZ,
					accel);
	}
	if (rc == 0) {
		rc = sensor_channel_get(dev, SENSOR_CHAN_GYRO_XYZ,
					gyro);
	}
	if (rc == 0) {
		rc = sensor_channel_get(dev, SENSOR_CHAN_DIE_TEMP,
					&temperature);
	}
	if (rc == 0) {
		printf("[%s]:%g Cel\n"
		       "  accel %f %f %f m/s/s\n"
		       "  gyro  %f %f %f rad/s\n",
		       now_str(),
		       sensor_value_to_double(&temperature),
		       sensor_value_to_double(&accel[0]),
		       sensor_value_to_double(&accel[1]),
		       sensor_value_to_double(&accel[2]),
		       sensor_value_to_double(&gyro[0]),
		       sensor_value_to_double(&gyro[1]),
		       sensor_value_to_double(&gyro[2]));
	} else {
		printf("sample fetch/get failed: %d\n", rc);
	}

	return rc;
}

#ifdef CONFIG_MPU6050_TRIGGER
static struct sensor_trigger trigger;

static void handle_mpu6050_drdy(const struct device *dev,
				const struct sensor_trigger *trig)
{
	int rc = process_mpu6050(dev);

	if (rc != 0) {
		printf("cancelling trigger due to failure: %d\n", rc);
		(void)sensor_trigger_set(dev, trig, NULL);
		return;
	}
}
#endif /* CONFIG_MPU6050_TRIGGER */

int imu_init(const struct device *mpu6050)
{
#ifdef CONFIG_MPU6050_TRIGGER
	trigger = (struct sensor_trigger) {
		.type = SENSOR_TRIG_DATA_READY,
		.chan = SENSOR_CHAN_ALL,
	};
	if (sensor_trigger_set(mpu6050, &trigger,
			       handle_mpu6050_drdy) < 0) {
		printf("Cannot configure trigger\n");
		return 0;
	}
	printk("Configured for triggered sampling.\n");

#endif
	return 0;
}

