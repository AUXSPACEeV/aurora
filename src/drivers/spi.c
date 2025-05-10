/**
 * @file spi.c
 * @brief Aurora SPI driver
 * 
 * @note The source code is derived from parts of
 * @ref https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
 *
 * Author: Maximilian Stephan @ Auxspace e.V.
 * Copyright (C) 2025 Auxspace e.V.
 */

#include <errno.h>

#include <aurora/compiler.h>
#include <aurora/spi.h>

/* Fileprivate structs and typedefs */

struct spi_drv_list {
    mutex_t mutex;
    struct list_head list;
};

/* Prototypes */

/**
 * @brief SPI DMA IRQ handler function
 * 
 * @param DMA_IRQ_num: number of DMA_IRQ
 * @param dma_hw_ints_p: pointer to HW interrupts
 */
static void in_spi_irq_handler(const uint DMA_IRQ_num, io_rw_32 *dma_hw_ints_p);

/**
 * @brief SPI DMA IRQ handler for DMA_IRQ_0
 */
static void __not_in_flash_func(spi_irq_handler_0)();

/**
 * @brief SPI DMA IRQ handler for DMA_IRQ_1
 */
static void __not_in_flash_func(spi_irq_handler_1)();

/* Fileprivate global variables */

static bool irqChannel1 = false;
static bool irqShared = false;

static struct spi_drv_list spi_drivers = {
    .list = LIST_HEAD_INIT(spi_drivers.list),
};

/* Driver function implementation */

void spi_drv_list_init(void)
{
    mutex_init(&spi_drivers.mutex);
}

/*----------------------------------------------------------------------------*/

size_t spi_get_num()
{
    size_t ret;

    mutex_enter_blocking(&spi_drivers.mutex);
    ret = list_count_nodes(&spi_drivers.list);
    mutex_exit(&spi_drivers.mutex);

    return ret;
}

/*----------------------------------------------------------------------------*/

struct spi_config *spi_get_by_num(size_t num)
{
    struct spi_config* cfg = NULL;
    int i;

    assert(num < spi_get_num());

    mutex_enter_blocking(&spi_drivers.mutex);
    list_for_each_entry(cfg, &spi_drivers.list, node) {
        if (i++ == num)
            break;
    }

    mutex_exit(&spi_drivers.mutex);

    return cfg;
}

/*----------------------------------------------------------------------------*/

static void in_spi_irq_handler(const uint DMA_IRQ_num, io_rw_32 *dma_hw_ints_p)
{
    for (size_t i = 0; i < spi_get_num(); ++i) {
        struct spi_config *config = spi_get_by_num(i);
        if (config == NULL) {
            continue;
        }
        struct spi_config_state *state = config->state;
        if (state == NULL) {
            continue;
        }

        if (DMA_IRQ_num == config->DMA_IRQ_num)  {
            // Is the SPI's channel requesting interrupt?
            if (*dma_hw_ints_p & (1 << state->rx_dma)) {
                *dma_hw_ints_p = 1 << state->rx_dma;  // Clear it.
                assert(!dma_channel_is_busy(state->rx_dma));
                assert(!sem_available(&state->sem));
                bool ok = sem_release(&state->sem);
                assert(ok);
            }
        }
    }
}

/*----------------------------------------------------------------------------*/

static void __not_in_flash_func(spi_irq_handler_0)()
{
    in_spi_irq_handler(DMA_IRQ_0, &dma_hw->ints0);
}

/*----------------------------------------------------------------------------*/

static void __not_in_flash_func(spi_irq_handler_1)()
{
    in_spi_irq_handler(DMA_IRQ_1, &dma_hw->ints1);
}

/*----------------------------------------------------------------------------*/

void set_spi_dma_irq_channel(bool useChannel1, bool shared)
{
    irqChannel1 = useChannel1;
    irqShared = shared;
}

/*----------------------------------------------------------------------------*/

int spi_transfer_dma(struct spi_config *spi, const uint8_t *tx, uint8_t* rx,
                     size_t length)
{
    // assert(512 == length || 1 == length);
    if (unlikely(!tx && !rx)) {
        return -EINVAL;
    }

    // tx write increment is already false
    if (tx) {
        channel_config_set_read_increment(&spi->state->tx_dma_cfg, true);
    } else {
        static const uint8_t dummy = 0xff;
        tx = &dummy;
        channel_config_set_read_increment(&spi->state->tx_dma_cfg, false);
    }

    // rx read increment is already false
    if (rx) {
        channel_config_set_write_increment(&spi->state->rx_dma_cfg, true);
    } else {
        static uint8_t dummy = 0xA5;
        rx = &dummy;
        channel_config_set_write_increment(&spi->state->rx_dma_cfg, false);
    }

    dma_channel_configure(spi->state->tx_dma, &spi->state->tx_dma_cfg,
                          &spi_get_hw(spi->hw_spi)->dr,  // write address
                          tx,      // read address
                          length,  // element count (each element is
                                   // of size transfer_data_size)
                          false);  // start
    dma_channel_configure(spi->state->rx_dma, &spi->state->rx_dma_cfg,
                          rx,                              // write address
                          &spi_get_hw(spi->hw_spi)->dr,  // read address
                          length,  // element count (each element is of size
                                   // transfer_data_size)
                          false);  // start

    switch (spi->DMA_IRQ_num) {
        case DMA_IRQ_0:
            assert(!dma_channel_get_irq0_status(spi->state->rx_dma));
            break;
        case DMA_IRQ_1:
            assert(!dma_channel_get_irq1_status(spi->state->rx_dma));
            break;
        default:
            assert(false);
    }
    sem_reset(&spi->state->sem, 0);

    // start them exactly simultaneously to avoid races (in extreme cases
    // the FIFO could overflow)
    dma_start_channel_mask(
    (1u << spi->state->tx_dma) | (1u << spi->state->rx_dma));

    /* Wait until master completes transfer or time out has occured. */
    uint32_t timeOut = 1000; /* Timeout 1 sec */
    bool rc = sem_acquire_timeout_ms(
    &spi->state->sem, timeOut);  // Wait for notification from ISR
    if (!rc) {
        // If the timeout is reached the function will return false
        printf("Notification wait timed out in %s\n", __FUNCTION__);
        return -ETIMEDOUT;
    }
    // Shouldn't be necessary:
    dma_channel_wait_for_finish_blocking(spi->state->tx_dma);
    dma_channel_wait_for_finish_blocking(spi->state->rx_dma);

    assert(!sem_available(&spi->state->sem));
    assert(!dma_channel_is_busy(spi->state->tx_dma));
    assert(!dma_channel_is_busy(spi->state->rx_dma));

    return 0;
}

/*----------------------------------------------------------------------------*/

int aurora_spi_init(struct spi_config *spi)
{
    auto_init_mutex(aurora_spi_init_mutex);
    mutex_enter_blocking(&aurora_spi_init_mutex);

    if (spi->state == NULL) {
        spi->state = malloc(sizeof(struct spi_config_state));
    }

    if (spi->state->initialized) {
        mutex_exit(&aurora_spi_init_mutex);
        return true;
    }

    //// The SPI may be shared (using multiple SSs); protect it
    //spi->mutex = xSemaphoreCreateRecursiveMutex();
    //xSemaphoreTakeRecursive(spi->mutex, portMAX_DELAY);
    if (!mutex_is_initialized(&spi->state->mutex)) {
        mutex_init(&spi->state->mutex);
    }
    spi_lock(spi);

    if (mutex_is_initialized(&spi_drivers.mutex) == false) {
        spi_drv_list_init();
    }

    // Default:
    if (!spi->baud_rate)
        spi->baud_rate = 10 * 1000 * 1000;
    // For the IRQ notification:
    sem_init(&spi->state->sem, 0, 1);

    /* Configure component */
    // Enable SPI at 100 kHz and connect to GPIOs
    spi_init(spi->hw_spi, 100 * 1000);
    spi_set_format(spi->hw_spi, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    gpio_set_function(spi->miso_gpio, GPIO_FUNC_SPI);
    gpio_set_function(spi->mosi_gpio, GPIO_FUNC_SPI);
    gpio_set_function(spi->sck_gpio, GPIO_FUNC_SPI);

    // TODO: Figure this out
    // bi_decl(bi_3pins_with_func(spi->miso_gpio, spi->mosi_gpio,
    //    spi->sck_gpio, GPIO_FUNC_SPI));

    // Slew rate limiting levels for GPIO outputs.
    // enum gpio_slew_rate { GPIO_SLEW_RATE_SLOW = 0, GPIO_SLEW_RATE_FAST = 1 }
    // void gpio_set_slew_rate (uint gpio,enum gpio_slew_rate slew)
    // Default appears to be GPIO_SLEW_RATE_SLOW.

    // Drive strength levels for GPIO outputs.
    if (spi->set_drive_strength) {
        gpio_set_drive_strength(spi->mosi_gpio, spi->mosi_gpio_drive_strength);
        gpio_set_drive_strength(spi->sck_gpio, spi->sck_gpio_drive_strength);
    }

    // SD cards' DO MUST be pulled up.
    gpio_pull_up(spi->miso_gpio);

    if (!spi->use_dma) {
        spi_unlock(spi);
        goto out;
    }

    // Grab some unused dma channels
    spi->state->tx_dma = dma_claim_unused_channel(true);
    spi->state->rx_dma = dma_claim_unused_channel(true);

    spi->state->tx_dma_cfg = dma_channel_get_default_config(
                                spi->state->tx_dma);
    spi->state->rx_dma_cfg = dma_channel_get_default_config(
                                spi->state->rx_dma);
    channel_config_set_transfer_data_size(&spi->state->tx_dma_cfg,
                                            DMA_SIZE_8);
    channel_config_set_transfer_data_size(&spi->state->rx_dma_cfg,
                                            DMA_SIZE_8);

    // We set the outbound DMA to transfer from a memory buffer to the SPI
    // transmit FIFO paced by the SPI TX FIFO DREQ The default is for the
    // read address to increment every element (in this case 1 byte -
    // DMA_SIZE_8) and for the write address to remain unchanged.
    channel_config_set_dreq(&spi->state->tx_dma_cfg,
                            spi_get_index(spi->hw_spi)
                                ? DREQ_SPI1_TX
                                : DREQ_SPI0_TX);
    channel_config_set_write_increment(&spi->state->tx_dma_cfg, false);

    // We set the inbound DMA to transfer from the SPI receive FIFO to a
    // memory buffer paced by the SPI RX FIFO DREQ We coinfigure the read
    // address to remain unchanged for each element, but the write address
    // to increment (so data is written throughout the buffer)
    channel_config_set_dreq(&spi->state->rx_dma_cfg,
                            spi_get_index(spi->hw_spi)
                                ? DREQ_SPI1_RX
                                : DREQ_SPI0_RX);
    channel_config_set_read_increment(&spi->state->rx_dma_cfg, false);

    /* Theory: we only need an interrupt on rx complete,
    since if rx is complete, tx must also be complete. */

    /* Configure the cpu to run dma_handler() when DMA IRQ 0/1 is asserted */

    spi->DMA_IRQ_num = irqChannel1 ? DMA_IRQ_1 : DMA_IRQ_0;

    // Tell the DMA to raise IRQ line 0/1 when the channel finishes a block
    static void (*spi_irq_handler_p)();
    switch (spi->DMA_IRQ_num) {
        case DMA_IRQ_0:
            spi_irq_handler_p = spi_irq_handler_0;
            dma_channel_set_irq0_enabled(spi->state->rx_dma, true);
            dma_channel_set_irq0_enabled(spi->state->tx_dma, false);
            break;
        case DMA_IRQ_1:
            spi_irq_handler_p = spi_irq_handler_1;
            dma_channel_set_irq1_enabled(spi->state->rx_dma, true);
            dma_channel_set_irq1_enabled(spi->state->tx_dma, false);
            break;
        default:
            assert(false);
    }
    if (irqShared) {
        irq_add_shared_handler(
            spi->DMA_IRQ_num, *spi_irq_handler_p,
            PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    } else {
        irq_set_exclusive_handler(spi->DMA_IRQ_num, *spi_irq_handler_p);
    }
    irq_set_enabled(spi->DMA_IRQ_num, true);

    spi_unlock(spi);

out:
    mutex_enter_blocking(&spi_drivers.mutex);
    spi->node = LIST_HEAD_INIT(spi->node);
    list_add_tail(&spi_drivers.list, &spi->node);
    mutex_exit(&spi_drivers.mutex);

    spi->state->initialized = true;

    mutex_exit(&aurora_spi_init_mutex);
    return true;
}

void aurora_spi_deinit(struct spi_config *spi)
{
    auto_init_mutex(aurora_spi_deinit_mutex);
    mutex_enter_blocking(&aurora_spi_deinit_mutex);

    if (!spi) {
        return;
    } else if (!spi->state->initialized) {
        return;
    }

    spi_lock(spi);

    mutex_enter_blocking(&spi_drivers.mutex);
    list_del(&spi->node);
    mutex_exit(&spi_drivers.mutex);
    
    spi->state->initialized = false;

    spi_unlock(spi);

    free(spi->state);

    mutex_exit(&aurora_spi_deinit_mutex);
}

/* [] END OF FILE */