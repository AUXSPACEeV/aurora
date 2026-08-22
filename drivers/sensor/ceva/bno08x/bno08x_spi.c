#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include "bno08x.h"

static int ceva_bno08x_spi_read(
    const void* context, uint8_t* buffer, uint16_t length) {
    const struct spi_dt_spec* spi = (const struct spi_dt_spec*)context;
    int ret;

    const struct spi_buf rx_buf = {.buf = buffer, .len = length};
    const struct spi_buf_set rx = {.buffers = &rx_buf, .count = 1};

    ret = spi_read_dt(spi, &rx);

    return ret;
}

static int ceva_bno08x_spi_write(
    const void* context, uint8_t* buffer, uint16_t length) {
    const struct spi_dt_spec* spi = (const struct spi_dt_spec*)context;
    int ret;

    const struct spi_buf tx_buf = {.buf = buffer, .len = length};
    const struct spi_buf_set tx = {.buffers = &tx_buf, .count = 1};

    ret = spi_write_dt(spi, &tx);

    return ret;
}

static int ceva_bno08x_spi_init(const void* context) {
    const struct spi_dt_spec* spi = (const struct spi_dt_spec*)context;

    if (spi_is_ready_dt(spi) == false) {
        return -ENODEV;
    }

    return 0;
}

static void ceva_bno086_spi_release(const void* context) {
    const struct spi_dt_spec* spi = (const struct spi_dt_spec*)context;

    spi_release_dt(spi);
}

const struct ceva_bno08x_bus_api ceva_bno08x_spi_bus_api = {
    .read = ceva_bno08x_spi_read,
    .write = ceva_bno08x_spi_write,
    .init = ceva_bno08x_spi_init,
    .release = ceva_bno086_spi_release,
};
