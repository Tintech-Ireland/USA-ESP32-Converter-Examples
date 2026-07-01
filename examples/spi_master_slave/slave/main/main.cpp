//
// spi_slave - slave half of the SPI master<->slave example for the USA2 board.
//
// Flash this to the SLAVE board; flash ../master to the MASTER board. Wiring and
// pin rationale are in the master's main.cpp and in board_pins.h:
//     MOSI = GPIO2, SCLK = GPIO5, MISO = GPIO7, CS = GPIO10
//
// The slave pre-loads its own 4-byte counter to send back on MISO and, on each
// transaction the master clocks, receives the master's counter on MOSI. It logs
// the value it received. spi_slave_transmit() blocks until the master drives a
// full transaction (CS asserted + clocked), so this loop is naturally paced by
// the master.
//
// Bus SPI2 (GPSPI2), mode 0.
//

#include <cstdio>
#include <cstring>
#include <cinttypes>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_slave.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "board_pins.h"

namespace {

constexpr char              TAG[]  = "spi_slave";
constexpr spi_host_device_t kHost  = SPI2_HOST;
constexpr int               kPkt   = 4;        // one little-endian uint32

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
    // Keep the transceiver 5 V rail OFF so the borrowed pins stay idle.
    gpio_reset_pin(Board::EN_5V);
    gpio_set_direction(Board::EN_5V, GPIO_MODE_OUTPUT);
    gpio_set_level(Board::EN_5V, 0);

    spi_bus_config_t bus = {};
    bus.mosi_io_num   = Board::SPI_MOSI;
    bus.miso_io_num   = Board::SPI_MISO;
    bus.sclk_io_num   = Board::SPI_SCLK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;

    spi_slave_interface_config_t slv = {};
    slv.spics_io_num = Board::SPI_CS;
    slv.mode         = 0;
    slv.queue_size   = 1;

    // A pull-up on CS keeps the slave deselected while the master is absent /
    // booting, avoiding spurious transactions.
    gpio_set_pull_mode(Board::SPI_CS, GPIO_PULLUP_ONLY);

    ESP_ERROR_CHECK(spi_slave_initialize(kHost, &bus, &slv, SPI_DMA_DISABLED));

    ESP_LOGI(TAG, "SPI slave up: SCLK=GPIO%d MOSI=GPIO%d MISO=GPIO%d CS=GPIO%d mode0",
             Board::SPI_SCLK, Board::SPI_MOSI, Board::SPI_MISO, Board::SPI_CS);

    uint32_t slave_counter = 0;
    for (;;) {
        const uint8_t tx[kPkt] = {
            static_cast<uint8_t>(slave_counter      ),
            static_cast<uint8_t>(slave_counter >>  8),
            static_cast<uint8_t>(slave_counter >> 16),
            static_cast<uint8_t>(slave_counter >> 24),
        };
        uint8_t rx[kPkt] = {0};

        spi_slave_transaction_t t = {};
        t.length    = kPkt * 8;     // in bits
        t.tx_buffer = tx;
        t.rx_buffer = rx;

        // Blocks until the master clocks a transaction.
        const esp_err_t err = spi_slave_transmit(kHost, &t, portMAX_DELAY);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "RX counter=%" PRIu32 "  (sent back %" PRIu32 ")", le32(rx), slave_counter);
            ++slave_counter;
        } else {
            ESP_LOGW(TAG, "transfer failed: %s", esp_err_to_name(err));
        }
    }
}
