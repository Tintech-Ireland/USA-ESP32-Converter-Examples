//
// i2c_master - master half of the I2C master<->slave example for the USA2 board.
//
// I2C is asymmetric, so this demo is split across two boards / two firmwares:
//   * this project  -> flash to the MASTER board
//   * ../slave       -> flash to the SLAVE board
//
// The board has no dedicated I2C pins; the link runs over the spare GPIO0/GPIO1
// header (I2C_SDA = GPIO0, I2C_SCL = GPIO1, see board_pins.h). Wire the two boards:
//     master SDA (GPIO0) <-> slave SDA (GPIO0)
//     master SCL (GPIO1) <-> slave SCL (GPIO1)
//     master GND         <-> slave GND
// I2C needs pull-ups on SDA/SCL: the master enables its internal (~45 kΩ) pull-ups
// here, which is adequate for a short test link at 100 kHz. For anything longer or
// faster, fit external 4.7 kΩ pull-ups to 3V3.
//
// Behaviour: the master writes a 4-byte little-endian counter to the slave once per
// second and logs the result (the slave logs each value it receives).
//
// Uses the ESP-IDF v5.x bus-based I2C master driver (driver/i2c_master.h).
//

#include <cstdio>
#include <cinttypes>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

#include "board_pins.h"

namespace {

constexpr char     TAG[]       = "i2c_master";
constexpr uint16_t kSlaveAddr  = 0x42;        // 7-bit slave address (must match ../slave)
constexpr uint32_t kSclHz      = 100000;      // 100 kHz standard mode
constexpr int      kPktBytes   = 4;           // one little-endian uint32 counter
constexpr TickType_t kTxPeriod = pdMS_TO_TICKS(1000);

} // namespace

extern "C" void app_main(void)
{
    // --- Create the master bus on the spare-header pins -----------------------
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port                   = I2C_NUM_0;
    bus_cfg.sda_io_num                 = Board::I2C_SDA;
    bus_cfg.scl_io_num                 = Board::I2C_SCL;
    bus_cfg.clk_source                 = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt          = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    i2c_master_bus_handle_t bus = nullptr;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    // --- Attach the slave as a device on the bus ------------------------------
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = kSlaveAddr;
    dev_cfg.scl_speed_hz    = kSclHz;

    i2c_master_dev_handle_t dev = nullptr;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &dev));

    ESP_LOGI(TAG, "I2C master up: SDA=GPIO%d SCL=GPIO%d addr=0x%02X @%" PRIu32 "Hz",
             Board::I2C_SDA, Board::I2C_SCL, kSlaveAddr, kSclHz);

    uint32_t counter = 0;
    for (;;) {
        const uint8_t buf[kPktBytes] = {
            static_cast<uint8_t>(counter      ),
            static_cast<uint8_t>(counter >>  8),
            static_cast<uint8_t>(counter >> 16),
            static_cast<uint8_t>(counter >> 24),
        };

        const esp_err_t err = i2c_master_transmit(dev, buf, kPktBytes, 1000);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "TX counter=%" PRIu32, counter);
            ++counter;
        } else {
            // Most commonly ESP_ERR_INVALID_STATE / timeout when the slave is
            // absent or unpowered (address not ACKed).
            ESP_LOGW(TAG, "TX failed: %s (slave present & wired?)", esp_err_to_name(err));
        }

        vTaskDelay(kTxPeriod);
    }
}
