/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <aurora/lib/baro.h>
#include <aurora/lib/can.h>
#include <aurora/lib/imu.h>
#include <zephyr/logging/log.h>

#define CAN_BARO_CHANNEL 0x200
#define CAN_IMU_GYRO_CHANNEL 0x201
#define CAN_IMU_ACCEL_CHANNEL 0x202
#define CAN_VOLTAGE_CHANNEL 0x203

#define BIT_MASK_FIRST 0b00000100
#define BIT_MASK_SECOND 0b00000010
#define BIT_MASK_THIRD 0b00000001

const uint8_t masks[3] = {BIT_MASK_FIRST, BIT_MASK_SECOND, BIT_MASK_THIRD};

// TODO: log level
LOG_MODULE_REGISTER(can, CONFIG_AURORA_SENSORS_LOG_LEVEL);

const struct device *const can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));

static inline void temp_to_sensor_value(int16_t raw_temp,
                                        struct sensor_value *val) {
  val->val1 = raw_temp / 100;
  val->val2 = (raw_temp % 100) * 10000;
}

static inline void press_to_sensor_value(uint32_t raw_press_pa,
                                         struct sensor_value *val) {
  val->val1 = raw_press_pa / 1000;
  val->val2 = (raw_press_pa % 1000) * 10000;
}

static inline void
imu_data_to_sensor_value(struct can_payload_imu *data,
                         struct sensor_value val[IMU_NUM_AXES]) {
  const uint16_t raw_coords[3] = {data->x, data->y, data->z};

  for (int i = 0; i < IMU_NUM_AXES; i++) {
    int sign = (data->negative_bits & masks[i]) ? -1 : 1;

    val[i].val1 = (int32_t)(raw_coords[i] / 100) * sign;
    val[i].val2 = (int32_t)(raw_coords[i] % 100) * 10000 * sign;
  }
}

static inline int extract_first_two_numbers(int32_t val) {
  int r = abs(val);
  while (r >= 100) {
    r /= 10;
  }

  return r;
}

static inline void
check_for_negative_values(struct sensor_value values[IMU_NUM_AXES],
                          int8_t *out) {
  for (int i = 0; i < IMU_NUM_AXES; i++) {
    if (values[i].val1 < 0 || values[i].val2 < 0) {
      *out = (*out) | masks[i];
    }
  }
}

static inline double out_ev(const struct sensor_value *val) {
  return (val->val1 + (double)val->val2 / 1000000);
}

static void tx_irq_callback(const struct device *dev, int error, void *arg) {

  ARG_UNUSED(dev);
  ARG_UNUSED(arg);

  if (error != 0) {
    LOG_ERR("Callback! error-code: %d", error);
  }
}

int init_can() {

  if (!device_is_ready(can_dev)) {
    LOG_ERR("CAN: Device %s not ready.", can_dev->name);
    return -ENODEV;
  }

#if defined(CONFIG_AURORA_CAN_LOCAL)
  // for local testing with one board
  int r = can_set_mode(can_dev, CAN_MODE_LOOPBACK);
  if (r != 0) {
    LOG_ERR("Error setting mode [%d]", r);
    return r;
  }
#endif

  int ret = can_start(can_dev);
  if (ret != 0) {
    LOG_WRN("Error starting CAN controller [%d]", ret);
  } else {
    LOG_INF("Started CAN controller");
  }

  return ret;
}

int can_send_msg(uint32_t id, const uint8_t *data, uint8_t dlc) {
  struct can_frame frame = {
      .id = id,
      .dlc = dlc,
      .flags = 0,
  };

  if (dlc > 8) {
    return -EINVAL;
  }

  memcpy(frame.data, data, dlc);

  // small timeout for sending consecutive frames in imu_msg
  int ret = can_send(can_dev, &frame, K_MSEC(10), tx_irq_callback, NULL);

  if (ret < 0) {
    LOG_WRN("Error sending data: %d", ret);
    return ret;
  }

  return 0;
}

int can_send_baro_msg(struct baro_data *baro) {
  // converting the sensor value to XXYY
  struct can_payload_baro p = {
      .temp_centi_deg = (int16_t)(baro->temperature.val1 * 100 +
                                  baro->temperature.val2 / 10000),
      // converting to XXYYY
      .press_pa =
          (uint32_t)(baro->pressure.val1 * 1000 + baro->pressure.val2 / 1000)};

  int r = can_send_msg(CAN_BARO_CHANNEL, (const uint8_t *)&p, sizeof(p));

  if (r < 0) {
    LOG_WRN("ERROR SENDING %d", r);
  }

  return r;
}

int can_send_imu_msg(struct imu_data *imu) {
  int8_t negative_bits_acc = 0;
  check_for_negative_values(imu->accel, &negative_bits_acc);

  int8_t negative_bits_gyo = 0;
  check_for_negative_values(imu->gyro, &negative_bits_gyo);

  struct can_payload_imu acc = {
      .x = (int16_t)(abs(imu->accel[0].val1 * 100) +
                     extract_first_two_numbers(imu->accel[0].val2)),
      .y = (int16_t)(abs(imu->accel[1].val1 * 100) +
                     extract_first_two_numbers(imu->accel[0].val2)),
      .z = (int16_t)(abs(imu->accel[2].val1 * 100) +
                     extract_first_two_numbers(imu->accel[0].val2)),
      .negative_bits = negative_bits_acc};

  struct can_payload_imu gyo = {
      .x = (int16_t)(abs(imu->gyro[0].val1 * 100) +
                     extract_first_two_numbers(imu->gyro[0].val2)),
      .y = (int16_t)(abs(imu->gyro[1].val1 * 100) +
                     extract_first_two_numbers(imu->gyro[1].val2)),
      .z = (int16_t)(abs(imu->gyro[2].val1 * 100) +
                     extract_first_two_numbers(imu->gyro[2].val2)),
      .negative_bits = negative_bits_gyo};

  int r =
      can_send_msg(CAN_IMU_ACCEL_CHANNEL, (const uint8_t *)&acc, sizeof(acc));

  if (r) {
    LOG_WRN("ERROR SENDING ACCEL DATA %d", r);
    return r;
  }

  r = can_send_msg(CAN_IMU_GYRO_CHANNEL, (const uint8_t *)&gyo, sizeof(gyo));

  if (r) {
    LOG_WRN("ERROR SENDING GYRO DATA %d", r);
  }

  return r;
}

/**
 * @brief Handles receving data. Currently just transforms the data back to
 * sensors values and logs them.
 *
 * @param dev
 * @param frame
 * @param user_data
 */
static void can_rx_callback(const struct device *dev, struct can_frame *frame,
                            void *user_data) {
  LOG_INF("RX ID: 0x%03x, DLC: %d", frame->id, frame->dlc);

  switch (frame->id) {
  case CAN_BARO_CHANNEL:
    struct can_payload_baro payload;
    memcpy(&payload, frame->data, sizeof(payload));
    struct sensor_value t;
    struct sensor_value p;
    temp_to_sensor_value(payload.temp_centi_deg, &t);
    press_to_sensor_value(payload.press_pa, &p);

    LOG_INF("RX BARO %2d.%2d °C; %2d.%3d kPascal", t.val1, t.val2, p.val1,
            p.val2);

    break;
  case CAN_IMU_ACCEL_CHANNEL:
    struct can_payload_imu payload_imu;
    memcpy(&payload_imu, frame->data, sizeof(payload_imu));
    struct imu_data v = {};
    imu_data_to_sensor_value(&payload_imu, v.accel);

    LOG_INF("RX ACC: X: %d.%d, Y: %d.%d, Z:%d.%d", v.accel[0].val1,
            v.accel[0].val2, v.accel[1].val1, v.accel[1].val2, v.accel[2].val1,
            v.accel[2].val2);

    break;

  case CAN_IMU_GYRO_CHANNEL:
    struct can_payload_imu payload_imu2;
    memcpy(&payload_imu2, frame->data, sizeof(payload_imu2));
    struct imu_data v2 = {};
    imu_data_to_sensor_value(&payload_imu2, v2.gyro);

    LOG_INF("RX GYO: X: %d.%d, Y: %d.%d, Z:%d.%d", v2.gyro[0].val1,
            v2.gyro[0].val2, v2.gyro[1].val1, v2.gyro[1].val2, v2.gyro[2].val1,
            v2.gyro[2].val2);

    break;
  case CAN_VOLTAGE_CHANNEL:
    // TODO

  default:
    LOG_WRN("Unknown Message ID %d", frame->id);
  }
}

int register_can_receiver() {
  const struct can_filter filter = {
      .id = 0x200,
      .mask = 0x7fc, // Listening on 0x200 - 0x203
      .flags = 0,
  };

  int filter_id = can_add_rx_filter(can_dev, can_rx_callback, NULL, &filter);
  if (filter_id < 0) {
    LOG_ERR("Failed to register filter: %d", filter_id);
  }

  return filter_id;
}

int can_send_voltage() {
  // TODO
  return 0;
}
int can_send_state() {
  // TODO
  return 0;
}
