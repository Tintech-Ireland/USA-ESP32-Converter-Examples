//
// rs232_to_rs232 - RS232 UART peer-to-peer example for the USA2 ESP32-C3 board.
//
// Drives UART1 through the on-board MAX3232 level shifter
// (RS232_TX = GPIO5, RS232_RX = GPIO4, see board_pins.h). The MAX3232 presents
// true RS232 levels on the TX_RS232 / RX_RS232 pins of connector J1.
//
// Usage: flash this firmware onto TWO boards and cross-connect them:
//     board A  TX_RS232  ->  board B  RX_RS232
//     board A  RX_RS232  <-  board B  TX_RS232
//     board A  GND       <-> board B  GND
// Each node sends a counter line once per second and prints whatever it receives.
//
// Framing: 115200 baud, 8 data bits, no parity, 1 stop bit (8N1).
//

#include <cstdio>
#include <cstring>
#include <cinttypes>   // PRIu32

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"

#include "board_pins.h"

namespace {

constexpr char TAG[] = "rs232";

constexpr uart_port_t kPort    = UART_NUM_1;
constexpr int         kBaud    = 115200;
constexpr int         kBufSize = 1024;

constexpr TickType_t kTxPeriod = pdMS_TO_TICKS(1000);

void tx_task(void*)
{
    uint32_t counter = 0;
    char line[64];

    for (;;) {
        const int n = std::snprintf(line, sizeof(line), "RS232 hello #%" PRIu32 "\n", counter);
        uart_write_bytes(kPort, line, n);
        ESP_LOGI(TAG, "TX: %.*s", n - 1, line);   // trim trailing newline in the log
        ++counter;
        vTaskDelay(kTxPeriod);
    }
}

void rx_task(void*)
{
    uint8_t buf[kBufSize];

    for (;;) {
        const int len = uart_read_bytes(kPort, buf, sizeof(buf) - 1, pdMS_TO_TICKS(200));
        if (len > 0) {
            buf[len] = '\0';
            ESP_LOGI(TAG, "RX (%d): %s", len, reinterpret_cast<char*>(buf));
        }
    }
}

} // namespace

extern "C" void app_main(void)
{
    const uart_config_t cfg = {
        .baud_rate  = kBaud,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(kPort, kBufSize * 2, 0, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(kPort, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(kPort, Board::RS232_TX, Board::RS232_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "RS232 UART1 up @%d 8N1 (TX=GPIO%d RX=GPIO%d)",
             kBaud, Board::RS232_TX, Board::RS232_RX);

    xTaskCreate(rx_task, "rs232_rx", 4096, nullptr, 9, nullptr);
    xTaskCreate(tx_task, "rs232_tx", 4096, nullptr, 8, nullptr);
}
