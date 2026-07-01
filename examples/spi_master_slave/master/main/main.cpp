//
// spi_master - master half of the SPI master<->slave example for the USA2 board.
//
// SPI is asymmetric, so this demo is split across two boards / two firmwares:
//   * this project  -> flash to the MASTER board
//   * ../slave       -> flash to the SLAVE board
//
// The board has no dedicated SPI pins; the four signals are spread across the
// spare header + borrowed transceiver nets (see board_pins.h). MISO shares
// RS485_RX (SP3485 RO, an output), so 5V_EN is held low to keep that transceiver
// powered down and off the bus:
//     MOSI = GPIO2 (spare), SCLK = GPIO5, MISO = GPIO7, CS = GPIO10
// Wire the two boards pin-for-pin (same signal to same signal) plus GND:
//     master MOSI <-> slave MOSI,  SCLK <-> SCLK,  MISO <-> MISO,
//     master CS   <-> slave CS,    GND  <-> GND
//
// Full-duplex behaviour: each second the master clocks a 4-byte little-endian
// counter out on MOSI while simultaneously shifting in the slave's counter on
// MISO. (The slave's value lags by one transaction, which is normal for an SPI
// slave that pre-loads its response.)
//
// Bus SPI2 (GPSPI2), 1 MHz, mode 0.
//

#include <cstdio>
#include <cstring>
#include <cinttypes>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "board_pins.h"

namespace {

constexpr char              TAG[]    = "spi_master";
constexpr spi_host_device_t kHost    = SPI2_HOST;
constexpr int               kPkt     = 4;          // one little-endian uint32
constexpr int               kClockHz = 1000000;    // 1 MHz
constexpr TickType_t        kPeriod  = pdMS_TO_TICKS(1000);

uint32_t le32(const uint8_t* b)
{
    return static_cast<uint32_t>(b[0])
         | static_cast<uint32_t>(b[1]) <<  8
         | static_cast<uint32_t>(b[2]) << 16
         | static_cast<uint32_t>(b[3]) << 24;
}

} // namespace

extern "C" void app_main(void)
{
    // Keep the transceiver 5 V rail OFF so the borrowed RS232/RS485 pins stay
    // idle and clear of the SPI bus.
    gpio_reset_pin(Board::EN_5V);
    gpio_set_direction(Board::EN_5V, GPIO_MODE_OUTPUT);
    gpio_set_level(Board::EN_5V, 0);

    spi_bus_config_t bus = {};
    bus.mosi_io_num     = Board::SPI_MOSI;
    bus.miso_io_num     = Board::SPI_MISO;
    bus.sclk_io_num     = Board::SPI_SCLK;
    bus.quadwp_io_num   = -1;
    bus.quadhd_io_num   = -1;
    bus.max_transfer_sz = 32;
    ESP_ERROR_CHECK(spi_bus_initialize(kHost, &bus, SPI_DMA_DISABLED));

    spi_device_interface_config_t dev = {};
    dev.clock_speed_hz = kClockHz;
    dev.mode           = 0;
    dev.spics_io_num   = Board::SPI_CS;
    dev.queue_size     = 1;

    spi_device_handle_t handle = nullptr;
    ESP_ERROR_CHECK(spi_bus_add_device(kHost, &dev, &handle));

    ESP_LOGI(TAG, "SPI master up: SCLK=GPIO%d MOSI=GPIO%d MISO=GPIO%d CS=GPIO%d @%dHz mode0",
             Board::SPI_SCLK, Board::SPI_MOSI, Board::SPI_MISO, Board::SPI_CS, kClockHz);

    uint32_t counter = 0;
    for (;;) {
        const uint8_t tx[kPkt] = {
            static_cast<uint8_t>(counter      ),
            static_cast<uint8_t>(counter >>  8),
            static_cast<uint8_t>(counter >> 16),
            static_cast<uint8_t>(counter >> 24),
        };
        uint8_t rx[kPkt] = {0};

        spi_transaction_t t = {};
        t.length    = kPkt * 8;     // in bits
        t.tx_buffer = tx;
        t.rx_buffer = rx;

        const esp_err_t err = spi_device_transmit(handle, &t);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "TX counter=%" PRIu32 "  RX(from slave)=%" PRIu32, counter, le32(rx));
            ++counter;
        } else {
            ESP_LOGW(TAG, "transfer failed: %s", esp_err_to_name(err));
        }

        vTaskDelay(kPeriod);
    }
}
