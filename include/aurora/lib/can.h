#include <aurora/lib/baro.h>
#include <aurora/lib/imu.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>
#include <aurora/lib/state/state.h>

/**
 * @brief Data structure for sending baro data.
 *
 * carries the temperature in the format XXYY, where XX are the whole number and
 * YY the decimals. The pressure is in the format XXYYY.
 */
struct __packed can_payload_baro {
  int16_t temp_centi_deg;
  uint32_t press_pa;
};

/**
 * @brief Data structure for sending imu data (gyro and accel).
 *
 * carries the measurement data from the IMU, either accelerometer or gyroscope
 * reading for the x, y and z axes. The negative bits indicate which values are
 * negative. 0b00000100 -> x is negative. The format for the x, y and z values
 * is XXYY, where XX are the whole numbers and YY the decimals.
 *
 */
struct __packed can_payload_imu {
  uint16_t x;
  uint16_t y;
  uint16_t z;
  int8_t negative_bits;
};

/**
 * @brief Initialize and start the CAN controller.
 *
 * Checks device readiness and starts the controller.
 *
 * @retval 0 on success.
 * @retval -ENODEV if the device is not ready.
 */
int init_can();

/**
 * @brief Splits the imu data into two payloads, one for gyro, one for accel,
 * and sends them on the CAN bus.
 *
 * @param imu Pointer to the IMU data.
 * @return int
 */
int can_send_imu_msg(struct imu_data *imu);

/**
 * @brief Sends the baro data over the CAN bus.
 *
 * @param baro Pointer to the baro data.
 * @retval see can_send
 */
int can_send_baro_msg(struct baro_data *baro);

/**
 * @brief Sends the voltage over the CAN bus.
 *
 * @retval see can_send
 */
int can_send_voltage();

/**
 * @brief Sends the current state over the CAN bus.
 *
 * @retval see can_send
 */
int can_send_state(enum sm_state state);

/**
 * @brief Sends any data over the CAN bus.
 *
 * @param id 	Channel-ID
 * @param data 	data to send
 * @param dlc 	lenght of the data
 * @retval 0 on success
 * @retval -E... Errors see can_send()
 */
int can_send_msg(uint32_t id, const uint8_t *data, uint8_t dlc);

/**
 * @brief
 *
 */
void can_rx_msg();

/**
 * @brief Registers the filters to listen to.
 *
 * @retval see can_add_rx_filter
 */
int register_can_receiver();
