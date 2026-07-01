//
// can_to_can - CAN (TWAI) peer-to-peer example for the USA2 ESP32-C3 board.
//
// The ESP32-C3 has a single TWAI (CAN 2.0) controller wired to the on-board
// TJA1042T-3 transceiver (CAN_TX = GPIO8, CAN_RX = GPIO6, see board_pins.h).
//
// Usage: flash this same firmware onto TWO boards, join their CANH/CANL lines
// (with a 120 ohm termination resistor at each end of the bus), and power both.
// Each board periodically transmits a counter frame and logs every frame it
// receives from the other node. This demonstrates a working CAN-to-CAN link.
//
// Bus speed: 500 kbit/s, standard 11-bit identifiers, normal (ACK) mode.
//

#include <cstdio>
#include <cinttypes>   // PRIx32 / PRIu32

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/twai.h"
#include "esp_log.h"

#include "board_pins.h"

namespace {

constexpr char TAG[] = "can_to_can";

// Arbitrary application message identifier for our heartbeat frame.
constexpr uint32_t kHeartbeatId = 0x100;

// How often each node transmits.
constexpr TickType_t kTxPeriod = pdMS_TO_TICKS(1000);

// ---------------------------------------------------------------------------
// Transmit task: emits an incrementing counter in an 8-byte standard frame.
// ---------------------------------------------------------------------------
void tx_task(void*)
{
    uint32_t counter = 0;

    for (;;) {
        twai_message_t msg = {};      // zero-initialise all flags
        msg.identifier       = kHeartbeatId;
        msg.data_length_code = 8;

        // Little-endian counter in the first 4 bytes; rest left as zero.
        msg.data[0] = static_cast<uint8_t>(counter      );
        msg.data[1] = static_cast<uint8_t>(counter >>  8);
        msg.data[2] = static_cast<uint8_t>(counter >> 16);
        msg.data[3] = static_cast<uint8_t>(counter >> 24);

        const esp_err_t err = twai_transmit(&msg, pdMS_TO_TICKS(1000));
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "TX  id=0x%03" PRIx32 "  counter=%" PRIu32, msg.identifier, counter);
            ++counter;
        } else {
            ESP_LOGW(TAG, "TX failed: %s", esp_err_to_name(err));
        }

        vTaskDelay(kTxPeriod);
    }
}

// ---------------------------------------------------------------------------
// Receive task: blocks on the RX queue and logs whatever arrives.
// ---------------------------------------------------------------------------
void rx_task(void*)
{
    for (;;) {
        twai_message_t msg = {};
        const esp_err_t err = twai_receive(&msg, portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "RX failed: %s", esp_err_to_name(err));
            continue;
        }

        uint32_t value = static_cast<uint32_t>(msg.data[0])
                       | static_cast<uint32_t>(msg.data[1]) <<  8
                       | static_cast<uint32_t>(msg.data[2]) << 16
                       | static_cast<uint32_t>(msg.data[3]) << 24;

        ESP_LOGI(TAG, "RX  id=0x%03" PRIx32 "  dlc=%d  value=%" PRIu32,
                 msg.identifier, msg.data_length_code, value);
    }
}

} // namespace

extern "C" void app_main(void)
{
    // Configure TWAI on the board's CAN pins, normal mode (participates in ACK).
    twai_general_config_t g_config =
        TWAI_GENERAL_CONFIG_DEFAULT(Board::CAN_TX, Board::CAN_RX, TWAI_MODE_NORMAL);
    twai_timing_config_t  t_config = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t  f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    ESP_ERROR_CHECK(twai_driver_install(&g_config, &t_config, &f_config));
    ESP_ERROR_CHECK(twai_start());
    ESP_LOGI(TAG, "TWAI started @500kbit/s  (TX=GPIO%d RX=GPIO%d)",
             Board::CAN_TX, Board::CAN_RX);

    xTaskCreate(rx_task, "can_rx", 4096, nullptr, 9, nullptr);
    xTaskCreate(tx_task, "can_tx", 4096, nullptr, 8, nullptr);
}
