#ifndef ZEPHYR_DRIVERS_SENSOR_CEVA_BNO08X_SPI_H_
#define ZEPHYR_DRIVERS_SENSOR_CEVA_BNO08X_SPI_H_

#include <zephyr/devicetree.h>
#include <zephyr/drivers/spi.h>
#include "bno08x.h"

/* The BNO08x SPI interface is mode 3 (CPOL=1, CPHA=1). */
#define CEVA_BNO08X_DEVICE_SPI_BUS(inst)                                   \
    extern const struct ceva_bno08x_bus_api ceva_bno08x_spi_bus_api;       \
                                                                           \
    static const struct spi_dt_spec spi_spec##inst = SPI_DT_SPEC_INST_GET( \
        inst,                                                              \
        SPI_WORD_SET(8) | SPI_MODE_CPOL | SPI_MODE_CPHA | SPI_HOLD_ON_CS | \
            SPI_LOCK_ON,                                                   \
        0);                                                                \
                                                                           \
    static const struct ceva_bno08x_bus ceva_bno08x_bus_api##inst = {      \
        .context = &spi_spec##inst, .api = &ceva_bno08x_spi_bus_api}

#endif /* ZEPHYR_DRIVERS_SENSOR_CEVA_BNO08X_SPI_H_ */
