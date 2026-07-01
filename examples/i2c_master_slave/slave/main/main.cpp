//
// i2c_slave - slave half of the I2C master<->slave example for the USA2 board.
//
// Flash this to the SLAVE board; flash ../master to the MASTER board. Wiring and
// pull-up notes are in the master's main.cpp. Link runs over the spare GPIO0/GPIO1
// header (I2C_SDA = GPIO0, I2C_SCL = GPIO1, see board_pins.h).
//
// Behaviour: the slave answers at address 0x42, receives the master's 4-byte
// little-endian counter, and logs each value.
//
// Uses the default (version 1) ESP-IDF I2C slave driver (driver/i2c_slave.h):
// a receive buffer is "armed" with i2c_slave_receive(); when it fills, the ISR
// callback on_recv_done() fires. The callback only hands the value to a queue;
// a task does the logging and re-arms the next receive (ISR context must stay
// lean and must not call logging APIs).
//

#include <cstdio>
#include <cinttypes>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2c_slave.h"
#include "esp_log.h"

#include "board_pins.h"

namespace {

constexpr char     TAG[]      = "i2c_slave";
constexpr uint16_t kSlaveAddr = 0x42;     // must match ../master
constexpr int      kPktBytes  = 4;        // one little-endian uint32 counter

QueueHandle_t s_rx_queue = nullptr;       // ISR -> task hand-off
uint8_t       s_rx_buf[kPktBytes];        // armed receive buffer (valid until callback)

// Runs in ISR context: decode the 4 received bytes and queue them. No logging here.
bool on_recv_done(i2c_slave_dev_handle_t,
                  const i2c_slave_rx_done_event_data_t* evt,
                  void*)
{
    BaseType_t higher_woken = pdFALSE;
    const uint32_t value = static_cast<uint32_t>(evt->buffer[0])
                         | static_cast<uint32_t>(evt->buffer[1]) <<  8
                         | static_cast<uint32_t>(evt->buffer[2]) << 16
                         | static_cast<uint32_t>(evt->buffer[3]) << 24;
    xQueueSendFromISR(s_rx_queue, &value, &higher_woken);
    return higher_woken == pdTRUE;        // let the driver yield if we woke a task
}

} // namespace

extern "C" void app_main(void)
{
    s_rx_queue = xQueueCreate(8, sizeof(uint32_t));

    i2c_slave_config_t cfg = {};
    cfg.i2c_port       = I2C_NUM_0;
    cfg.sda_io_num     = Board::I2C_SDA;
    cfg.scl_io_num     = Board::I2C_SCL;
    cfg.clk_source     = I2C_CLK_SRC_DEFAULT;
    cfg.send_buf_depth = 64;
    cfg.slave_addr     = kSlaveAddr;
    cfg.addr_bit_len   = I2C_ADDR_BIT_LEN_7;

    i2c_slave_dev_handle_t slave = nullptr;
    ESP_ERROR_CHECK(i2c_new_slave_device(&cfg, &slave));

    i2c_slave_event_callbacks_t cbs = {};
    cbs.on_recv_done = on_recv_done;
    ESP_ERROR_CHECK(i2c_slave_register_event_callbacks(slave, &cbs, nullptr));

    ESP_LOGI(TAG, "I2C slave up: SDA=GPIO%d SCL=GPIO%d addr=0x%02X",
             Board::I2C_SDA, Board::I2C_SCL, kSlaveAddr);

    // Arm the first receive; each completed transfer is re-armed in the loop below.
    ESP_ERROR_CHECK(i2c_slave_receive(slave, s_rx_buf, kPktBytes));

    uint32_t value = 0;
    for (;;) {
        if (xQueueReceive(s_rx_queue, &value, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "RX counter=%" PRIu32, value);
            i2c_slave_receive(slave, s_rx_buf, kPktBytes);   // re-arm for the next packet
        }
    }
}
