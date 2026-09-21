#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/byteorder.h>
#include <aurora/lib/baro.h>
#include <aurora/lib/imu.h>


struct __packed can_payload_baro {
    int16_t temp_centi_deg;
    uint32_t press_pa;
};

struct __packed can_payload_imu {
    uint16_t x;
    uint16_t y;
    uint16_t z;
    int8_t negative_bits; // bits indicating which values are negative 0b0000 0101 -> x and z are negative
};


int init_can();
int can_send_imu_msg(struct imu_data *imu);
int can_send_baro_msg(struct baro_data *baro);
int can_send_voltage();
int can_send_state();

int can_send_msg(uint32_t id, const uint8_t *data, uint8_t dlc);
void can_rx_msg();
int register_can_receiver();
