//
// serial_selftest - build/smoke test for the serial_transport component.
//
// Brings up UART1 on the RS232 pins, wraps it in a SerialPort::UartByteSerial,
// and drives a SimpleProtocol::SerialProtocol over it. Its purpose is to prove
// the serial_transport component compiles, links, and runs on the ESP32-C3 and
// to serve as the usage template for the upcoming WiFi/Bluetooth converters.
//
// With two boards (TX_RS232/RX_RS232 crossed, GND common) and one built as
// master and the other as slave, this exchanges framed, CRC-checked packets.
// On its own it simply reports receive() timeouts, which still exercises the
// whole stack.
//
// NOTE: SimpleProtocol::ReceiveResult is ~1 KB, so the protocol task is given an
// 8 KB stack.
//

#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"

#include "board_pins.h"
#include "uart_byte_serial.h"
#include "simprot.h"

namespace {

constexpr char        TAG[]   = "serial_selftest";
constexpr uart_port_t kPort   = UART_NUM_1;
constexpr int         kBaud   = 115200;
constexpr int         kBufLen = 1024;

// Build as master by default; flash the second board with kMaster=false.
constexpr bool        kMaster = true;

void protocol_task(void*)
{
    static SerialPort::UartByteSerial    bytes(kPort);
    static SimpleProtocol::SerialProtocol proto(&bytes, kMaster);

    ESP_LOGI(TAG, "serial_transport up on UART1 (%s): framing + CRC16 + ENQ/ACK",
             kMaster ? "master" : "slave");

    uint32_t counter = 0;
    for (;;) {
        if (kMaster) {
            char msg[32];
            const int n = std::snprintf(msg, sizeof(msg), "hello #%u", static_cast<unsigned>(counter));
            const int rc = proto.send(reinterpret_cast<const unsigned char*>(msg),
                                      static_cast<size_t>(n), 1000);
            if (rc == BaseDefs::BD_NO_ERROR) {
                ESP_LOGI(TAG, "sent '%s' (acked)", msg);
                ++counter;
            } else {
                ESP_LOGW(TAG, "send rc=%d (no peer yet?)", rc);
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        } else {
            SimpleProtocol::ReceiveResult r = proto.receive(1000);
            if (r.length >= 0) {
                ESP_LOGI(TAG, "received %d bytes: %.*s", r.length, r.length, r.data.data());
            }
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

    ESP_ERROR_CHECK(uart_driver_install(kPort, kBufLen * 2, 0, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(kPort, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(kPort, Board::RS232_TX, Board::RS232_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    xTaskCreate(protocol_task, "simprot", 8192, nullptr, 8, nullptr);
}
