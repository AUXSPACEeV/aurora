/*
 * Copyright (c) 2024 Sensorimotor Learning Lab (SML-Posey), MIT-licensed.
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: MIT
 *
 * Vendored from SML-Posey/posey-firmware and adapted for AURORA. The original
 * driver exposed the BNO08x only through an SH-2/SHTP callback API. This
 * version keeps that core (SPI transport, SH-2 HAL, IRQ-serviced work queue)
 * and adds, on top of it:
 *   - report enabling at init (accel / calibrated gyro / calibrated magnetic
 *     field) at a configurable interval, and
 *   - a Zephyr sensor_driver_api facade: the SH-2 sensor callback latches the
 *     newest reports, sample_fetch is a no-op (data is kept fresh by the IRQ),
 *     and channel_get returns them as SENSOR_CHAN_ACCEL/GYRO/MAGN_XYZ.
 * so the in-tree AURORA IMU library consumes it with no changes.
 */

#include "bno08x.h"
#include "bno08x_spi.h"

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/devicetree.h>

#include "sh2/sh2.h"
#include "sh2/sh2_SensorValue.h"
#include "sh2/sh2_err.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ceva_bno08x);

#define DT_DRV_COMPAT ceva_bno08x

/* uTesla -> Gauss (Zephyr SENSOR_CHAN_MAGN_* unit). */
#define BNO08X_UT_TO_GAUSS (0.01)

// For some reason this is hidden in shtp.c.
#define SHTP_HDR_LEN (4)

static int ceva_bno08x_bus_init(const struct device* dev) {
    const struct ceva_bno08x_config* config =
        (const struct ceva_bno08x_config*)dev->config;

    const struct ceva_bno08x_bus* bus = config->bus;

    return bus->api->init(bus->context);
}

static int ceva_bno08x_bus_read(
    const struct device* dev, uint8_t* buffer, uint16_t length) {
    const struct ceva_bno08x_config* config =
        (const struct ceva_bno08x_config*)dev->config;

    const struct ceva_bno08x_bus* bus = config->bus;

    return bus->api->read(bus->context, buffer, length);
}

static int ceva_bno08x_bus_write(
    const struct device* dev, uint8_t* buffer, uint16_t length) {
    const struct ceva_bno08x_config* config =
        (const struct ceva_bno08x_config*)dev->config;

    const struct ceva_bno08x_bus* bus = config->bus;

    return bus->api->write(bus->context, buffer, length);
}

static void ceva_bno08x_bus_release(const struct device* dev) {
    const struct ceva_bno08x_config* config =
        (const struct ceva_bno08x_config*)dev->config;
    const struct ceva_bno08x_bus* bus = config->bus;
    return bus->api->release(bus->context);
}

void ceva_bno08x_hardware_reset(const struct device* dev) {
    const struct ceva_bno08x_config* config =
        (const struct ceva_bno08x_config*)dev->config;

    LOG_WRN("Resetting IMU...");

    gpio_pin_set_dt(&config->reset_gpio, 0);
    k_sleep(K_MSEC(15));
    gpio_pin_set_dt(&config->reset_gpio, 1);
    k_sleep(K_MSEC(15));
    gpio_pin_set_dt(&config->reset_gpio, 0);
    k_sleep(K_MSEC(15));
}

bool ceva_bno08x_int_active(const struct device* dev) {
    const struct ceva_bno08x_config* config =
        (const struct ceva_bno08x_config*)dev->config;
    return gpio_pin_get_dt(&config->int_gpio) == 1;
}

bool ceva_bno08x_wait_for_int(const struct device* dev) {
    static const int MaxIters = 500;
    for (int i = 0; i < MaxIters; i++) {
        if (ceva_bno08x_int_active(dev)) {
            return true;
        }

        k_msleep(1);
    }

    // Timed out.
    ceva_bno08x_hardware_reset(dev);
    LOG_ERR("Did not find nINT!");

    return false;
}

//---------------------------------------------------------//
// Wrappers for the SH2 HAL interface.

static void ceva_bno08x_sh2_hal_sensor_callback(
    void* cookie, sh2_SensorEvent_t* event) {
    if (cookie == NULL)
        return;
    struct ceva_bno08x_data* data = ((struct device*)cookie)->data;
    if (data->sh2_sensor_value == NULL)
        return;

    int rc = sh2_decodeSensorEvent(data->sh2_sensor_value, event);
    if (rc != SH2_OK) {
        data->sh2_sensor_value->timestamp = 0;
    }
}

static void ceva_bno08x_sh2_hal_event_callback(
    void* cookie, sh2_AsyncEvent_t* pEvent) {
    if (cookie == NULL)
        return;
    struct device* dev = (struct device*)cookie;
    struct ceva_bno08x_data* data = (struct ceva_bno08x_data*)dev->data;

    // Call a user-defined handler here if set.
    if (data->user_event_handler != NULL) {
        data->user_event_handler(dev, pEvent);
    }
}

static int ceva_bno08x_sh2_hal_open(sh2_Hal_t* self) {
    // This function initializes communications with the device.  It
    // can initialize any GPIO pins and peripheral devices used to
    // interface with the sensor hub.
    // It should also perform a reset cycle on the sensor hub to
    // ensure communications start from a known state.

    struct ceva_bno08x_sh2_hal* hal = (struct ceva_bno08x_sh2_hal*)self;

    LOG_INF("Resetting BNO08x device...");
    ceva_bno08x_hardware_reset(hal->dev);

    return 0;
}

static void ceva_bno08x_sh2_hal_close(sh2_Hal_t* self) {
    // This function completes communications with the sensor hub.
    // It should put the device in reset then de-initialize any
    // peripherals or hardware resources that were used.

    // Nothing to do.
}

static uint32_t ceva_bno08x_sh2_hal_get_time_us(sh2_Hal_t* self) {
    // This function should return a 32-bit value representing a
    // microsecond counter.  The count may roll over after 2^32
    // microseconds.
    //
    // k_cycle_get_32() returns hardware cycles (what k_cyc_to_us_floor32
    // expects); the SH-2 open handshake busy-waits on the delta of this
    // against ADVERT_TIMEOUT_US, so it must advance in real microseconds.
    return k_cyc_to_us_floor32(k_cycle_get_32());
}

static int ceva_bno08x_sh2_hal_read(
    sh2_Hal_t* self, uint8_t* pBuffer, unsigned len, uint32_t* t_us) {
    // This function supports reading data from the sensor hub.
    // It will be called frequently to service the device.
    //
    // If the HAL has received a full SHTP transfer, this function
    // should load the data into pBuffer, set the timestamp to the
    // time the interrupt was detected, and return the non-zero length
    // of data in this transfer.
    //
    // If the HAL has not recevied a full SHTP transfer, this function
    // should return 0.
    struct ceva_bno08x_sh2_hal* hal = (struct ceva_bno08x_sh2_hal*)self;

    uint16_t packet_size = 0;
    uint32_t time = ceva_bno08x_sh2_hal_get_time_us(self);

    if (!ceva_bno08x_int_active(hal->dev)) {
        return 0;
    }

    if (ceva_bno08x_bus_read(hal->dev, pBuffer, SHTP_HDR_LEN) < 0) {
        return 0;
    }

    // Determine amount to read
    packet_size = (uint16_t)pBuffer[0] | (uint16_t)pBuffer[1] << 8;

    if (packet_size == 0xFFFF) {
        ceva_bno08x_bus_release(hal->dev);
        return 0;
    }

    // Unset the "continue" bit
    packet_size &= ~0x8000;

    if ((packet_size > len) || (packet_size == 0)) {
        ceva_bno08x_bus_release(hal->dev);
        return 0;
    }

    if (packet_size > SHTP_HDR_LEN) {
        if (ceva_bno08x_bus_read(
                hal->dev, &pBuffer[SHTP_HDR_LEN], packet_size - SHTP_HDR_LEN) <
            0) {
            ceva_bno08x_bus_release(hal->dev);
            return 0;
        }
    }

    ceva_bno08x_bus_release(hal->dev);

    if (t_us)
        *t_us = time;
    return packet_size;
}

static int ceva_bno08x_sh2_hal_write(
    sh2_Hal_t* self, uint8_t* pBuffer, unsigned len) {
    // This function supports writing data to the sensor hub.
    struct ceva_bno08x_sh2_hal* hal = (struct ceva_bno08x_sh2_hal*)self;
    struct ceva_bno08x_config* config =
        (struct ceva_bno08x_config*)hal->dev->config;

    if (!ceva_bno08x_wait_for_int(hal->dev)) {
        gpio_pin_set_dt(&config->ps0_gpio, 1);
        return 0;
    }

    gpio_pin_set_dt(&config->ps0_gpio, 1);

    if (ceva_bno08x_bus_write(hal->dev, pBuffer, len) < 0) {
        ceva_bno08x_bus_release(hal->dev);
        return 0;
    }

    ceva_bno08x_bus_release(hal->dev);

    return len;
}

//---------------------------------------------------------//
// Sensor-API facade: latch SH-2 reports, serve Zephyr channels.

/* SH-2 sensor callback: convert the newest decoded report into Zephyr units
 * and latch it under the data lock, for channel_get to read. Runs on the
 * system workqueue (see ceva_bno08x_work_handler). */
static void ceva_bno08x_latch_sensor(
    const struct device* dev, const sh2_SensorValue_t* value) {
    struct ceva_bno08x_data* data = (struct ceva_bno08x_data*)dev->data;

    k_mutex_lock(&data->lock, K_FOREVER);
    switch (value->sensorId) {
    case SH2_ACCELEROMETER: /* [m/s^2] */
        data->accel.x = value->un.accelerometer.x;
        data->accel.y = value->un.accelerometer.y;
        data->accel.z = value->un.accelerometer.z;
        data->have_accel = true;
        break;
    case SH2_GYROSCOPE_CALIBRATED: /* [rad/s] */
        data->gyro.x = value->un.gyroscope.x;
        data->gyro.y = value->un.gyroscope.y;
        data->gyro.z = value->un.gyroscope.z;
        data->have_gyro = true;
        break;
    case SH2_MAGNETIC_FIELD_CALIBRATED: /* [uTesla] -> [gauss] */
        data->magn.x = value->un.magneticField.x * BNO08X_UT_TO_GAUSS;
        data->magn.y = value->un.magneticField.y * BNO08X_UT_TO_GAUSS;
        data->magn.z = value->un.magneticField.z * BNO08X_UT_TO_GAUSS;
        data->have_magn = true;
        break;
    default:
        break;
    }
    k_mutex_unlock(&data->lock);
}

static void ceva_bno08x_vec3_to_sensor_value(
    const struct ceva_bno08x_vec3* v, struct sensor_value* val) {
    sensor_value_from_double(&val[0], (double)v->x);
    sensor_value_from_double(&val[1], (double)v->y);
    sensor_value_from_double(&val[2], (double)v->z);
}

/* The reports are pushed asynchronously by the hub and latched in the IRQ
 * work handler, so a fetch has nothing to do; channel_get returns the newest
 * latched sample. */
static int ceva_bno08x_sample_fetch(
    const struct device* dev, enum sensor_channel chan) {
    ARG_UNUSED(dev);
    ARG_UNUSED(chan);
    return 0;
}

static int ceva_bno08x_channel_get(
    const struct device* dev, enum sensor_channel chan,
    struct sensor_value* val) {
    struct ceva_bno08x_data* data = (struct ceva_bno08x_data*)dev->data;
    int ret = 0;

    k_mutex_lock(&data->lock, K_FOREVER);
    switch (chan) {
    case SENSOR_CHAN_ACCEL_XYZ:
        if (!data->have_accel) {
            ret = -ENODATA;
            break;
        }
        ceva_bno08x_vec3_to_sensor_value(&data->accel, val);
        break;
    case SENSOR_CHAN_GYRO_XYZ:
        if (!data->have_gyro) {
            ret = -ENODATA;
            break;
        }
        ceva_bno08x_vec3_to_sensor_value(&data->gyro, val);
        break;
    case SENSOR_CHAN_MAGN_XYZ:
        if (!data->have_magn) {
            ret = -ENODATA;
            break;
        }
        ceva_bno08x_vec3_to_sensor_value(&data->magn, val);
        break;
    default:
        ret = -ENOTSUP;
        break;
    }
    k_mutex_unlock(&data->lock);

    return ret;
}

static const struct sensor_driver_api ceva_bno08x_api = {
    .sample_fetch = ceva_bno08x_sample_fetch,
    .channel_get = ceva_bno08x_channel_get,
};

/* Enable the accel / calibrated gyro / calibrated magnetic-field reports. */
static int ceva_bno08x_enable_reports(const struct device* dev) {
    const struct ceva_bno08x_config* config =
        (const struct ceva_bno08x_config*)dev->config;
    static const uint8_t sensors[] = {
        SH2_ACCELEROMETER,
        SH2_GYROSCOPE_CALIBRATED,
        SH2_MAGNETIC_FIELD_CALIBRATED,
    };
    sh2_SensorConfig_t cfg;
    int rc;

    memset(&cfg, 0, sizeof(cfg));
    cfg.reportInterval_us = config->report_interval_us;

    for (size_t i = 0; i < ARRAY_SIZE(sensors); i++) {
        rc = sh2_setSensorConfig(sensors[i], &cfg);
        if (rc != SH2_OK) {
            LOG_WRN("Failed to enable report 0x%02x (%d)", sensors[i], rc);
            return -EIO;
        }
    }

    return 0;
}

//---------------------------------------------------------//

static void ceva_bno08x_irq_callback(const struct device* dev) {
    struct ceva_bno08x_data* data = (struct ceva_bno08x_data*)dev->data;
    k_work_submit(&data->callback_work);
}

static int ceva_bno08x_init_irq(const struct device* dev) {
    struct ceva_bno08x_data* data = (struct ceva_bno08x_data*)dev->data;
    struct ceva_bno08x_config* config = (struct ceva_bno08x_config*)dev->config;
    int ret;

    gpio_init_callback(
        &data->gpio_callback, config->int_gpio_callback,
        BIT(config->int_gpio.pin));

    ret = gpio_add_callback(config->int_gpio.port, &data->gpio_callback);

    if (ret < 0) {
        return ret;
    }

    return gpio_pin_interrupt_configure_dt(
        &config->int_gpio, GPIO_INT_EDGE_TO_ACTIVE);
}

void ceva_bno08x_work_handler(struct ceva_bno08x_data* data) {
    sh2_SensorValue_t value;

    data->sh2_sensor_value = &value;

    while (true) {
        value.timestamp = 0;
        sh2_service();
        if (value.timestamp == 0)
            break;

        // Call a user-defined handler here if set.
        if (data->user_sensor_handler != NULL) {
            data->user_sensor_handler(data->dev, &value);
        }
    }

    data->sh2_sensor_value = NULL;
}

static void ceva_bno08x_irq_work_handler(struct k_work* item) {
    struct ceva_bno08x_data* data =
        CONTAINER_OF(item, struct ceva_bno08x_data, callback_work);

    ceva_bno08x_work_handler(data);
}

// True iff spec->port is a ready device AND is a GPIO controller. The
// gpio_is_ready_dt() short-circuits (handling a NULL/not-ready port) before
// DEVICE_API_IS() dereferences port->api, so this is safe for any spec whose
// port is non-NULL when ready.
static bool ceva_bno08x_gpio_ctlr_ok(const struct gpio_dt_spec* spec) {
    return gpio_is_ready_dt(spec) && DEVICE_API_IS(gpio, spec->port);
}

static int ceva_bno08x_init(const struct device* dev) {
    struct ceva_bno08x_data* data = (struct ceva_bno08x_data*)dev->data;
    const struct ceva_bno08x_config* config =
        (const struct ceva_bno08x_config*)dev->config;
    int ret;

    // Initialize device data.
    k_mutex_init(&data->lock);
    k_work_init(&data->callback_work, ceva_bno08x_irq_work_handler);
    data->dev = dev;
    data->sh2_sensor_value = NULL;
    data->user_event_handler = NULL;
    data->user_sensor_handler = ceva_bno08x_latch_sensor;
    data->have_accel = false;
    data->have_gyro = false;
    data->have_magn = false;

    // Fail gracefully (rather than fault) if any control line's GPIO
    // controller is not ready, or is not actually a GPIO controller.
    //
    // gpio_is_ready_dt() only checks that spec->port is a ready device; it does
    // NOT check that the device is a GPIO controller. A *-gpios phandle that
    // points at a non-GPIO node (e.g. a bus or display controller) is a ready
    // device, so it slips through gpio_is_ready_dt(), and then the very first
    // gpio_pin_configure_dt() trips the DEVICE_API_IS(gpio, port) assert inside
    // gpio_pin_configure() -> kernel panic at init ("device API is not gpio").
    // Validate the controller class here so a mis-wired overlay returns -ENODEV
    // and logs instead of faulting.
    if (!ceva_bno08x_gpio_ctlr_ok(&config->reset_gpio) ||
        !ceva_bno08x_gpio_ctlr_ok(&config->ps0_gpio) ||
        !ceva_bno08x_gpio_ctlr_ok(&config->int_gpio) ||
        (config->ps1_gpio.port != NULL &&
         !ceva_bno08x_gpio_ctlr_ok(&config->ps1_gpio))) {
        LOG_ERR("GPIO control line not ready or not a GPIO controller");
        return -ENODEV;
    }

    // Initialize reset GPIO.
    ret = gpio_pin_configure_dt(&config->reset_gpio, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        LOG_WRN("Failed to init reset");
        return ret;
    }

    // Configure protocol select (PS) GPIOs for SPI (PS = {1,1}). PS1 may be
    // strapped high in hardware, in which case its spec has a NULL port.
    ret = gpio_pin_configure_dt(&config->ps0_gpio, GPIO_OUTPUT_ACTIVE);
    if (ret < 0) {
        LOG_WRN("Failed to init ps0");
        return ret;
    }
    if (config->ps1_gpio.port != NULL) {
        ret = gpio_pin_configure_dt(&config->ps1_gpio, GPIO_OUTPUT_ACTIVE);
        if (ret < 0) {
            LOG_WRN("Failed to init ps1");
            return ret;
        }
    }

    // Initialize interrupt GPIO, needed to wait for the sensor before the
    // interrupt is configured.
    ret = gpio_pin_configure_dt(&config->int_gpio, GPIO_INPUT);
    if (ret < 0) {
        return ret;
    }

    // Initialize the bus.
    ret = ceva_bno08x_bus_init(dev);
    if (ret < 0) {
        LOG_WRN("Failed to init bus");

        return ret;
    }

    // Reset the device and configure SH2.
    sh2_Hal_t* sh2_hal = (sh2_Hal_t*)config->hal;

    LOG_INF("Opening SH2...");
    ret = sh2_open(sh2_hal, ceva_bno08x_sh2_hal_event_callback, (void*)dev);
    if (ret < 0) {
        LOG_WRN("Failed to open sh2");
        return ret;
    }

    // Check connection partially by getting the product id's
    LOG_INF("Fetching product IDs...");
    sh2_ProductIds_t prodIds;
    memset(&prodIds, 0, sizeof(prodIds));
    ret = sh2_getProdIds(&prodIds);
    if (ret != SH2_OK) {
        LOG_WRN("Failed to get product ids");

        return ret;
    }
    LOG_INF("Found %d products:", prodIds.numEntries);
    for (int n = 0; n < prodIds.numEntries; n++) {
        LOG_INF(
            "  [%d] Part: <%x> SW: %d.%d.%d", n + 1,
            prodIds.entry[n].swPartNumber, prodIds.entry[n].swVersionMajor,
            prodIds.entry[n].swVersionMinor, prodIds.entry[n].swVersionPatch);
    }

    // Register sensor listener
    ret =
        sh2_setSensorCallback(ceva_bno08x_sh2_hal_sensor_callback, (void*)dev);
    if (ret != SH2_OK) {
        LOG_WRN("Failed to set sensor callback");

        return ret;
    }

    // Enable the reports the AURORA IMU library consumes. Done before the IRQ
    // is wired: sh2_setSensorConfig() is synchronous and pumps the SH-2 HAL
    // itself, so servicing it from the IRQ work handler concurrently would
    // reenter the (non-reentrant) sh2 library.
    LOG_INF("Enabling reports (%u us)...", config->report_interval_us);
    ret = ceva_bno08x_enable_reports(dev);
    if (ret < 0) {
        LOG_WRN("Failed to enable reports");
        return ret;
    }

    // Initialize interrupt GPIO and trigger.
    LOG_INF("Initializing IRQ...");
    ret = ceva_bno08x_init_irq(dev);
    if (ret < 0) {
        LOG_WRN("Failed to init irq");
        return ret;
    }

    sh2_service();

    LOG_INF("Done: ret=%d", ret);

    return ret;
}

/*
 * Currently only support for the SPI bus is implemented.
 */
#define CEVA_BNO08X_DEVICE_BUS(inst)                              \
    BUILD_ASSERT(DT_INST_ON_BUS(inst, spi), "Unimplemented bus"); \
    CEVA_BNO08X_DEVICE_SPI_BUS(inst)

#define CEVA_BNO08X_DEVICE(inst)                                               \
    static struct ceva_bno08x_data ceva_bno08x_data_##inst;                    \
                                                                               \
    CEVA_BNO08X_DEVICE_BUS(inst);                                              \
                                                                               \
    static void ceva_bno08x_irq_callback##inst(                                \
        const struct device* dev, struct gpio_callback* cb, uint32_t pins) {   \
        ceva_bno08x_irq_callback(DEVICE_DT_INST_GET(inst));                    \
    }                                                                          \
                                                                               \
    static const struct ceva_bno08x_sh2_hal ceva_bno08x_sh2_hal_##inst = {     \
        .sh2_hal =                                                             \
            {                                                                  \
                .open = ceva_bno08x_sh2_hal_open,                              \
                .close = ceva_bno08x_sh2_hal_close,                            \
                .read = ceva_bno08x_sh2_hal_read,                              \
                .write = ceva_bno08x_sh2_hal_write,                            \
                .getTimeUs = ceva_bno08x_sh2_hal_get_time_us,                  \
            },                                                                 \
        .dev = DEVICE_DT_INST_GET(inst),                                       \
    };                                                                         \
                                                                               \
    static const struct ceva_bno08x_config ceva_bno08x_config_##inst = {       \
        .bus = &ceva_bno08x_bus_api##inst,                                     \
        .hal = &ceva_bno08x_sh2_hal_##inst,                                    \
        .reset_gpio = GPIO_DT_SPEC_INST_GET(inst, reset_gpios),                \
        .ps0_gpio = GPIO_DT_SPEC_INST_GET(inst, ps0_gpios),                    \
        .ps1_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, ps1_gpios, {0}),            \
        .int_gpio = GPIO_DT_SPEC_INST_GET(inst, int_gpios),                    \
        .int_gpio_callback = ceva_bno08x_irq_callback##inst,                   \
        .report_interval_us = DT_INST_PROP(inst, report_interval_us),          \
    };                                                                         \
                                                                               \
    SENSOR_DEVICE_DT_INST_DEFINE(                                              \
        inst, ceva_bno08x_init, NULL, &ceva_bno08x_data_##inst,                \
        &ceva_bno08x_config_##inst, POST_KERNEL, 99, &ceva_bno08x_api);

DT_INST_FOREACH_STATUS_OKAY(CEVA_BNO08X_DEVICE)
