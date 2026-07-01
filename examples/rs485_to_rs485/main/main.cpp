//
// rs485_to_rs485 - RS485 half-duplex peer-to-peer example for the USA2 board.
//
// Drives UART1 through the on-board SP3485EN transceiver
// (RS485_TX = GPIO9, RS485_RX = GPIO7, RS485_DE = GPIO10, see board_pins.h).
// Direction control (SP3485 U2, schematic sheet 5/6): ~RE is hard-wired to GND
// (R9 = 0 ohm), so the receiver is ALWAYS enabled; DE is driven by RS485_TXEN
// (GPIO10) and is ACTIVE-HIGH (HIGH = transmit, LOW = receive). The IDF UART
// driver's RS485 half-duplex mode drives its RTS->DE pin high during transmit and
// low otherwise, which matches this exactly - no manual GPIO toggling required.
//
// Usage: flash this firmware onto TWO (or more) boards and bus them together:
//     all A_RS485 <-> A_RS485,  all B_RS485 <-> B_RS485,  GND <-> GND
// The board already carries termination (~100 ohm each; ~50 ohm with two boards,
// a healthy load). Each node sends a counter line once per second and prints what
// it receives.
//
// Framing: 115200 baud, 8N1.
//
// NOTE: RS485_TX (GPIO9) is an ESP32-C3 strapping pin and also carries the BOOT
// button; keep the bus from holding it low at reset. (RS485_RX is GPIO7, not a
// strapping pin.)
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

constexpr char TAG[] = "rs485";

constexpr uart_port_t kPort    = UART_NUM_1;
constexpr int         kBaud    = 115200;
constexpr int         kBufSize = 1024;

constexpr TickType_t kTxPeriod = pdMS_TO_TICKS(1000);

void tx_task(void*)
{
    uint32_t counter = 0;
    char line[64];

    for (;;) {
        const int n = std::snprintf(line, sizeof(line), "RS485 hello #%" PRIu32 "\n", counter);

        // In RS485 half-duplex mode the driver raises DE, sends, then drops DE.
        uart_write_bytes(kPort, line, n);
        // Block until the last bit has physically left the shifter before the
        // driver releases the bus - avoids truncating the frame on the wire.
        uart_wait_tx_done(kPort, pdMS_TO_TICKS(100));

        ESP_LOGI(TAG, "TX: %.*s", n - 1, line);
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

    // TX=GPIO9, RX=GPIO7, RTS drives the DE pin (GPIO10), CTS unused.
    ESP_ERROR_CHECK(uart_set_pin(kPort, Board::RS485_TX, Board::RS485_RX,
                                 Board::RS485_DE, UART_PIN_NO_CHANGE));

    // Hardware auto-controls the DE line around each transmission.
    ESP_ERROR_CHECK(uart_set_mode(kPort, UART_MODE_RS485_HALF_DUPLEX));

    ESP_LOGI(TAG, "RS485 UART1 up @%d 8N1 (TX=GPIO%d RX=GPIO%d DE=GPIO%d)",
             kBaud, Board::RS485_TX, Board::RS485_RX, Board::RS485_DE);

    xTaskCreate(rx_task, "rs485_rx", 4096, nullptr, 9, nullptr);
    xTaskCreate(tx_task, "rs485_tx", 4096, nullptr, 8, nullptr);
}
